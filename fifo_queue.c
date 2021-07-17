#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

enum { Q_OK, Q_ERROR };

typedef struct _list_t {
    /* pointer list: a pointer array */
    void **list;
    /* size of list */
    int size;
    /* number of pointers in rlist */
    int num;
} list_t;

typedef struct _hzp_rec_t {
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
} hzp_rec_t;

typedef void (*RELEASE_HZP_FUNC)(void *hzp);
typedef struct _hzp_rec_mgr_t {
    _Atomic hzp_rec_t *hzp_rec_head;
    int exp_thrd_num;
    int hzp_num_per_rec;
    int rlist_init_size;
    int scan_threshold;
    RELEASE_HZP_FUNC rls_hzp_func;
} hzp_rec_mgr_t;

#define CAS(addr, old, new) atomic_compare_exchange_weak((addr), (old), (new))

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

typedef struct NODE { /* Queue node */
    _Atomic void *value;
    _Atomic struct NODE *next;
} node_t;

typedef struct { /* Two lock queue */
    _Atomic node_t *first;
    _Atomic node_t *last;
    hzp_rec_mgr_t *hzp_mgr;
} con_queue_t;

typedef struct {
    hzp_rec_mgr_t *hzp_mgr;
    hzp_rec_t *my_rec;
} con_queue_thrd_t;

/* Free the queue struct. It assumes that the queue is depleted, and it will
 * not manage allocated elements inside of it.
 */
void con_free(con_queue_t *);

inline static node_t *_con_node_init(void *value)
{
    node_t *node = malloc(sizeof(node_t));
    if (!node)
        return NULL;
    atomic_init(&node->value, value);
    atomic_init(&node->next, NULL);

    return node;
}

void con_thrd_free(con_queue_t *queue, con_queue_thrd_t *restrict thrd_ctx)
{
    if (thrd_ctx) {
        hzp_rec_mgr_scan(thrd_ctx->hzp_mgr, thrd_ctx->my_rec);
        hzp_rec_mgr_put_myrec(thrd_ctx->hzp_mgr, thrd_ctx->my_rec);
        free(thrd_ctx);
    }
}

con_queue_thrd_t *con_thrd_init(con_queue_t *queue)
{
    if (!queue) {
        return NULL;
    }

    /* Allocate queue thrd ctx */
    con_queue_thrd_t *thrd = malloc(sizeof(con_queue_thrd_t));
    if (!thrd) {
        return NULL;
    }

    thrd->hzp_mgr = queue->hzp_mgr;
    thrd->my_rec = hzp_rec_mgr_get_myrec(queue->hzp_mgr);
    // TODO: error handling
    assert(thrd->my_rec != NULL);

    return thrd;
}

/* Allocates and initializes queue.
 * Returns a pointer to an allocated struct for the synchronized queue or NULL
 * on failure.
 */
con_queue_t *con_init()
{
    /* Allocate queue */
    con_queue_t *queue = malloc(sizeof(con_queue_t));
    if (!queue)
        return NULL;

    node_t *dummy = _con_node_init(NULL);
    if (!dummy) {
        con_free(queue);
        return NULL;
    }
    atomic_init(&queue->first, (_Atomic node_t *) dummy);
    atomic_init(&queue->last, (_Atomic node_t *) dummy);
    queue->hzp_mgr = hzp_rec_mgr_init(9, 2, free);
    // TODO: error handling
    assert(queue->hzp_mgr != NULL);

    return queue;
}

void con_free(con_queue_t *queue)
{
    if (!queue) {
        return;
    }

    if (queue->first) {
        free(queue->first);
    }

    hzp_rec_mgr_free(queue->hzp_mgr);

    free(queue);
}

/* Add element to queue. The client is responsible for freeing elementsput into
 * the queue afterwards. Returns Q_OK on success or Q_ERROR on failure.
 */
int con_push(con_queue_t *restrict queue,
             con_queue_thrd_t *restrict thrd_ctx,
             void *restrict new_element)
{
    /* Prepare new node */
    node_t *node = _con_node_init(new_element);
    if (!node) {
        return Q_ERROR;
    }

    hzp_rec_t *rec = thrd_ctx->my_rec;
    node_t *last = 0;
    node_t *next = 0;
    while (1) {
        last = (node_t *) atomic_load(&queue->last);
        rec->hzp[0] = last;
        if (last != (node_t *) atomic_load(&queue->last)) {
            continue;
        }
        next = (node_t *) atomic_load(&last->next);
        if (last != (node_t *) atomic_load(&queue->last)) {
            continue;
        }
        if (next) {
            CAS(&queue->last, &last, (_Atomic node_t *) next);
            continue;
        }
        /* next is NULL */
        if (CAS(&last->next, &next, (_Atomic node_t *) node)) {
            break;
        }
    }
    CAS(&queue->last, &last, (_Atomic node_t *) node);
    rec->hzp[0] = NULL;

    return Q_OK;
}

/* Retrieve element and remove it from the queue.
 * Returns a pointer to the element previously pushed in or NULL of the queue is
 * emtpy.
 */
void *con_pop(con_queue_t *queue, con_queue_thrd_t *restrict thrd_ctx)
{
    node_t *first = NULL;
    node_t *last = NULL;
    node_t *next = NULL;
    void *return_value = NULL;

    hzp_rec_t *rec = thrd_ctx->my_rec;
    while (1) {
        first = (node_t *) atomic_load(&queue->first);
        rec->hzp[0] = first;
        if (first != (node_t *) atomic_load(&queue->first)) {
            continue;
        }
        last = (node_t *) atomic_load(&queue->last);
        next = (node_t *) atomic_load(&first->next);
        rec->hzp[1] = next;
        if (first != (node_t *) atomic_load(&queue->first)) {
            continue;
        }
        if (!next) {
            rec->hzp[0] = NULL;
            rec->hzp[1] = NULL;
            return NULL;
        }
        if (first == last) {
            CAS(&queue->last, &last, (_Atomic node_t *) next);
            continue;
        }
        return_value = (void *) atomic_load(&(next->value));
        if (CAS(&queue->first, &first, (_Atomic node_t *) next)) {
            break;
        }
    }
    hzp_rec_mgr_retire_hzp(thrd_ctx->hzp_mgr, rec, first);
    rec->hzp[0] = NULL;
    rec->hzp[1] = NULL;
    return return_value;
}

#define N_PUSH_THREADS 4
#define N_POP_THREADS 4
#define NUM 1000000

/* This thread writes integers into the queue */
int push_thread(void *queue_ptr)
{
    con_queue_t *queue = (con_queue_t *) queue_ptr;
    con_queue_thrd_t *thrd_ctx = con_thrd_init(queue);
    assert(thrd_ctx != NULL);

    /* Push ints into queue */
    for (int i = 0; i < NUM; ++i) {
        int *pushed_value = malloc(sizeof(int));
        *pushed_value = i;
        if (con_push(queue, thrd_ctx, pushed_value) != Q_OK)
            printf("Error pushing element %i\n", i);
    }

    con_thrd_free(queue, thrd_ctx);

    thrd_exit(0);
}

typedef struct { /* context for pop_thread */
    con_queue_t *queue;
    int *recv_num;
} pop_ctx_t;

/* This thread reads ints from the queue and frees them */
int pop_thread(void *ctx_ptr)
{
    pop_ctx_t *ctx = (pop_ctx_t *) ctx_ptr;
    con_queue_thrd_t *thrd_ctx = con_thrd_init(ctx->queue);
    assert(thrd_ctx != NULL);

    /* Read values from queue. Break loop on -1 */
    while (1) {
        int *popped_value = (int *) con_pop(ctx->queue, thrd_ctx);
        if (popped_value) {
            if (*popped_value == -1) {
                printf("receive kill signal\n");
                free(popped_value);
                break;
            }
            if (*popped_value >= NUM || *popped_value < 0) {
                printf("recv unexpeteced value (%d)\n", *popped_value);
            } else {
                ctx->recv_num[*popped_value] += 1;
            }
            free(popped_value);
        } else {
            // recv_num[NUM] stores number of received NULL
            ctx->recv_num[NUM] += 1;
        }
    }

    con_thrd_free(ctx->queue, thrd_ctx);

    thrd_exit(0);
}

static bool check_pop_num(pop_ctx_t *ctx)
{
    bool ret = true;

    for (int n = 0; n < NUM; ++n) {
        int cnt = 0;
        for (int i = 0; i < N_POP_THREADS; ++i) {
            cnt += ctx[i].recv_num[n];
        }
        if (cnt != 4) {
            printf("recv number of %d isn't 4, but is %d\n", n, cnt);
            ret = false;
        }
    }

    return ret;
}

int main()
{
    thrd_t push_threads[N_PUSH_THREADS], pop_threads[N_POP_THREADS];
    pop_ctx_t pop_ctx[N_POP_THREADS];

    con_queue_t *queue = con_init();
    con_queue_thrd_t *thrd_ctx = con_thrd_init(queue);

    assert(queue != NULL);
    assert(thrd_ctx != NULL);

    for (int i = 0; i < N_PUSH_THREADS; ++i) {
        if (thrd_create(&push_threads[i], push_thread, queue) != thrd_success)
            printf("Error creating push thread %i\n", i);
    }

    for (int i = 0; i < N_POP_THREADS; ++i) {
        pop_ctx[i].queue = queue;
        pop_ctx[i].recv_num = (int *) calloc(NUM + 1, sizeof(int));

        if (thrd_create(&pop_threads[i], pop_thread, &(pop_ctx[i])) !=
            thrd_success)
            printf("Error creating pop thread %i\n", i);
    }
    printf("after creating all threads\n");

    for (int i = 0; i < N_PUSH_THREADS; ++i) {
        if (thrd_join(push_threads[i], NULL) != thrd_success)
            continue;
    }
    printf("after joining all push threads\n");

    /* Push kill signals */
    for (int i = 0; i < N_POP_THREADS; ++i) {
        int *kill_signal = malloc(sizeof(int));
        /* signal pop threads to exit */
        *kill_signal = -1;
        con_push(queue, thrd_ctx, kill_signal);
    }
    printf("after pushing all kill signals\n");

    for (int i = 0; i < N_POP_THREADS; ++i) {
        if (thrd_join(pop_threads[i], NULL) != thrd_success)
            continue;
    }
    printf("after joining all pop threads\n");

    int failed = 0;
    if (!check_pop_num(pop_ctx)) {
        failed = -1;
    }

    for (int i = 0; i < N_POP_THREADS; ++i) {
        printf("pop thread (%d) recv %d NULL.\n", i, pop_ctx[i].recv_num[NUM]);
        free(pop_ctx[i].recv_num);
    }

    con_thrd_free(queue, thrd_ctx);
    con_free(queue);
    return failed;
}