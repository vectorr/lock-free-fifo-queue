#include "fifo_queue.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "hzp_rec_mgr.h"

#define CAS(addr, old, new) atomic_compare_exchange_weak((addr), (old), (new))

typedef struct _node_t { /* Queue node */
    void *value;
    _Atomic struct _node_t *next;
} node_t;

struct _con_queue_t {
    _Atomic node_t *first;
    _Atomic node_t *last;
    hzp_rec_mgr_t *hzp_mgr;
};

struct _con_queue_thrd_t {
    hzp_rec_mgr_t *hzp_mgr;
    hzp_rec_t *my_rec;
};

inline static node_t *_con_node_init(void *value)
{
    node_t *node = malloc(sizeof(node_t));
    if (!node) {
        return NULL;
    }

    node->value = value;
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
        hzp_rec_set_pointer(rec, 0, last);
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
    hzp_rec_set_pointer(rec, 0, NULL);

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
        hzp_rec_set_pointer(rec, 0, first);
        if (first != (node_t *) atomic_load(&queue->first)) {
            continue;
        }
        last = (node_t *) atomic_load(&queue->last);
        next = (node_t *) atomic_load(&first->next);
        hzp_rec_set_pointer(rec, 1, next);
        if (first != (node_t *) atomic_load(&queue->first)) {
            continue;
        }
        if (!next) {
            hzp_rec_set_pointer(rec, 0, NULL);
            hzp_rec_set_pointer(rec, 1, NULL);
            return NULL;
        }
        if (first == last) {
            CAS(&queue->last, &last, (_Atomic node_t *) next);
            continue;
        }
        return_value = next->value;
        if (CAS(&queue->first, &first, (_Atomic node_t *) next)) {
            break;
        }
    }
    hzp_rec_mgr_retire_hzp(thrd_ctx->hzp_mgr, rec, first);
    hzp_rec_set_pointer(rec, 0, NULL);
    hzp_rec_set_pointer(rec, 1, NULL);

    return return_value;
}