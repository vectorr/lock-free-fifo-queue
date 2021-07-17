#ifndef _HAZARD_POINTER_MANAGER_H_
#define _HAZARD_POINTER_MANAGER_H_

#include <stdbool.h>

typedef struct _hzp_rec_mgr_t hzp_rec_mgr_t;
typedef struct _hzp_rec_t hzp_rec_t;
typedef void (*RELEASE_HZP_FUNC)(void *hzp);

hzp_rec_mgr_t *hzp_rec_mgr_init(int exp_thrd_num,
                                int hzp_num_per_rec,
                                RELEASE_HZP_FUNC rls_func);

void hzp_rec_mgr_free(hzp_rec_mgr_t *mgr);

hzp_rec_t *hzp_rec_mgr_get_myrec(hzp_rec_mgr_t *mgr);

void hzp_rec_mgr_put_myrec(hzp_rec_mgr_t *mgr, hzp_rec_t *my_rec);

void hzp_rec_mgr_retire_hzp(hzp_rec_mgr_t *restrict mgr,
                            hzp_rec_t *restrict my_rec,
                            void *retired_p);

void hzp_rec_mgr_scan(hzp_rec_mgr_t *restrict mgr, hzp_rec_t *my_rec);

bool hzp_rec_set_pointer(hzp_rec_t *rec, int idx, void *pointer);

#endif /* _HAZARD_POINTER_MANAGER_H_ */