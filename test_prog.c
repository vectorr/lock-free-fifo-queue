#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include "fifo_queue.h"

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