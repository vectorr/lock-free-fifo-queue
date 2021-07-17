#ifndef _FIFO_QUEUE_H_
#define _FIFO_QUEUE_H_

enum { Q_OK, Q_ERROR };

typedef struct _con_queue_t con_queue_t;
typedef struct _con_queue_thrd_t con_queue_thrd_t;

con_queue_t *con_init();

/* Free the queue struct. It assumes that the queue is depleted, and it will
 * not manage allocated elements inside of it.
 */
void con_free(con_queue_t *queue);

con_queue_thrd_t *con_thrd_init(con_queue_t *queue);

void con_thrd_free(con_queue_t *queue, con_queue_thrd_t *restrict thrd_ctx);

/* Add element to queue. The client is responsible for freeing elementsput into
 * the queue afterwards. Returns Q_OK on success or Q_ERROR on failure.
 */
int con_push(con_queue_t *restrict queue,
             con_queue_thrd_t *restrict thrd_ctx,
             void *restrict new_element);

/* Retrieve element and remove it from the queue.
 * Returns a pointer to the element previously pushed in or NULL of the queue is
 * emtpy.
 */
void *con_pop(con_queue_t *queue, con_queue_thrd_t *restrict thrd_ctx);

#endif /* _FIFO_QUEUE_H_ */