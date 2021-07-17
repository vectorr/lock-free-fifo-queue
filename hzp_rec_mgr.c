#include "hzp_rec_mgr.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CAS(addr, old, new) atomic_compare_exchange_weak((addr), (old), (new))

typedef struct _list_t {
    /* pointer list: a pointer array */
    void **list;
    /* size of list */
    int size;
    /* number of pointers in rlist */
    int num;
} list_t;

struct _hzp_rec_t {
    _Atomic struct _hzp_rec_t *next;
    _Atomic bool active;
    /* retired hazard pointers list */
    list_t rlist;
    /* for collecting all hazards pointers when scanning */
    list_t plist;
    /* number of hazard pointers in this record */
    int hzp_num;
    /* array of hazard pointers */
    void *hzp[1];
};

struct _hzp_rec_mgr_t {
    _Atomic hzp_rec_t *hzp_rec_head;
    int exp_thrd_num;
    int hzp_num_per_rec;
    int rlist_init_size;
    int scan_threshold;
    RELEASE_HZP_FUNC rls_hzp_func;
};


void _hzp_rec_free(hzp_rec_t *rec)
{
    if (rec) {
        if (rec->rlist.list) {
            free(rec->rlist.list);
        }
        if (rec->plist.list) {
            free(rec->plist.list);
        }
        free(rec);
    }
}

hzp_rec_t *_hzp_rec_init(int hzp_num, int rlist_init_sz, int exp_thrd_num)
{
    hzp_rec_t *rec = malloc(sizeof(hzp_rec_t) + sizeof(void *) * (hzp_num - 1));
    if (!rec) {
        return NULL;
    }
    atomic_init(&rec->next, NULL);
    atomic_init(&rec->active, false);

    /* initiate rlist */
    rec->rlist.size = rlist_init_sz;
    rec->rlist.list = calloc(rec->rlist.size, sizeof(void *));
    if (!rec->rlist.list) {
        _hzp_rec_free(rec);
        return NULL;
    }
    rec->rlist.num = 0;

    /* initiate plist */
    rec->plist.size = exp_thrd_num * hzp_num;
    rec->plist.list = calloc(rec->plist.size, sizeof(void *));
    if (!rec->plist.list) {
        _hzp_rec_free(rec);
        return NULL;
    }
    rec->plist.num = 0;

    /* initiate hzps */
    rec->hzp_num = hzp_num;
    for (int i; i < hzp_num; ++i) {
        rec->hzp[i] = NULL;
    }

    return rec;
}

#define RLIST_INIT_SIZE 32
#define TRIGGER_SCAN_THRESHOLD 16
hzp_rec_mgr_t *hzp_rec_mgr_init(int exp_thrd_num,
                                int hzp_num_per_rec,
                                RELEASE_HZP_FUNC rls_func)
{
    hzp_rec_mgr_t *mgr = malloc(sizeof(hzp_rec_mgr_t));
    if (!mgr) {
        return NULL;
    }

    atomic_init(&mgr->hzp_rec_head, NULL);
    mgr->exp_thrd_num = exp_thrd_num;
    mgr->hzp_num_per_rec = hzp_num_per_rec;
    mgr->rlist_init_size = RLIST_INIT_SIZE;
    mgr->scan_threshold = TRIGGER_SCAN_THRESHOLD;
    mgr->rls_hzp_func = rls_func;

    /* prepare exp_thrd_num hzp_recs at init */
    for (int i = 0; i < mgr->exp_thrd_num; ++i) {
        hzp_rec_t *rec = _hzp_rec_init(mgr->hzp_num_per_rec,
                                       mgr->rlist_init_size, mgr->exp_thrd_num);
        // TODO: error handling
        assert(rec != NULL);
        atomic_store(&rec->next, mgr->hzp_rec_head);
        atomic_store(&mgr->hzp_rec_head, (_Atomic hzp_rec_t *) rec);
    }

    return mgr;
}

static bool _is_in_array(void *check, void *array[], int array_size)
{
    for (int i = 0; i < array_size; ++i) {
        if (check == array[i]) {
            return true;
        }
    }
    return false;
}

static void _collect_hzps_in_all_recs_into_plist(hzp_rec_mgr_t *restrict mgr,
                                                 list_t *plist)
{
    plist->num = 0;
    for (hzp_rec_t *rec = (hzp_rec_t *) atomic_load(&mgr->hzp_rec_head); rec;
         rec = (hzp_rec_t *) atomic_load(&rec->next)) {
        // TODO: check active or not?
        for (int i = 0; i < mgr->hzp_num_per_rec; ++i) {
            void *hzp = rec->hzp[i];
            if (hzp) {
                // TODO: enlarge plist if needed
                assert(plist->num < plist->size);
                plist->list[plist->num++] = hzp;
            }
        }
    }
}

static void _release_hzps_in_rlist_but_not_in_plist(hzp_rec_mgr_t *restrict mgr,
                                                    list_t *rlist,
                                                    list_t *plist)
{
    int i = 0;
    while (i < rlist->num) {
        void *hzp = rlist->list[i];
        if (!_is_in_array(hzp, plist->list, plist->num)) {
            if (mgr->rls_hzp_func) {
                mgr->rls_hzp_func(hzp);
            }
            rlist->list[i] = rlist->list[rlist->num - 1];
            rlist->list[rlist->num - 1] = NULL;
            rlist->num--;
            continue;
        }
        ++i;
    }
}

void hzp_rec_mgr_scan(hzp_rec_mgr_t *restrict mgr, hzp_rec_t *my_rec)
{
    _collect_hzps_in_all_recs_into_plist(mgr, &my_rec->plist);
    _release_hzps_in_rlist_but_not_in_plist(mgr, &my_rec->rlist,
                                            &my_rec->plist);
}

void hzp_rec_mgr_retire_hzp(hzp_rec_mgr_t *restrict mgr,
                            hzp_rec_t *restrict my_rec,
                            void *retired_p)
{
    // TODO: enlarge rlist if needed
    assert(my_rec->rlist.num < my_rec->rlist.size);
    my_rec->rlist.list[my_rec->rlist.num++] = retired_p;
    for (int i = 0; i < mgr->hzp_num_per_rec; ++i) {
        if (my_rec->hzp[i] == retired_p) {
            my_rec->hzp[i] = NULL;
        }
    }
    if (my_rec->rlist.num >= mgr->scan_threshold) {
        hzp_rec_mgr_scan(mgr, my_rec);
    }
}

hzp_rec_t *hzp_rec_mgr_get_myrec(hzp_rec_mgr_t *mgr)
{
    hzp_rec_t *my_rec = NULL;

    /* search for inactive hzp_rec in queue first */
    for (my_rec = (hzp_rec_t *) atomic_load(&mgr->hzp_rec_head); my_rec;
         my_rec = (hzp_rec_t *) atomic_load(&my_rec->next)) {
        bool active_false = false;
        if (atomic_load(&my_rec->active)) {
            continue;
        }
        if (!CAS(&my_rec->active, &active_false, true)) {
            continue;
        }
        return my_rec;
    }

    my_rec = _hzp_rec_init(mgr->hzp_num_per_rec, mgr->rlist_init_size,
                           mgr->exp_thrd_num);
    if (!my_rec) {
        return NULL;
    }
    atomic_store(&my_rec->active, true);

    while (1) {
        hzp_rec_t *old_head = (hzp_rec_t *) atomic_load(&mgr->hzp_rec_head);
        my_rec->next = (_Atomic hzp_rec_t *) old_head;
        if (CAS(&mgr->hzp_rec_head, &old_head, (_Atomic hzp_rec_t *) my_rec)) {
            break;
        }
    }

    return my_rec;
}

bool hzp_rec_set_pointer(hzp_rec_t *rec, int idx, void *pointer)
{
    if (idx < 0 || idx >= rec->hzp_num) {
        return false;
    }
    rec->hzp[idx] = pointer;
    return true;
}

void hzp_rec_mgr_put_myrec(hzp_rec_mgr_t *mgr, hzp_rec_t *my_rec)
{
    for (int i = 0; i < mgr->hzp_num_per_rec; ++i) {
        my_rec->hzp[i] = NULL;
    }
    atomic_store(&my_rec->active, false);
}

void hzp_rec_mgr_free(hzp_rec_mgr_t *mgr)
{
    if (!mgr) {
        return;
    }
    if (mgr->hzp_rec_head) {
        hzp_rec_t *head = (hzp_rec_t *) mgr->hzp_rec_head;
        list_t *plist = &head->plist;

        /* using plist from first hzp_rec */
        _collect_hzps_in_all_recs_into_plist(mgr, plist);

        hzp_rec_t *rec = head;
        while (rec) {
            _release_hzps_in_rlist_but_not_in_plist(mgr, &rec->rlist,
                                                    &rec->plist);
            rec = (hzp_rec_t *) rec->next;
        }

        while (head) {
            rec = head;
            head = (hzp_rec_t *) rec->next;
            _hzp_rec_free(rec);
        }
    }
    free(mgr);
}