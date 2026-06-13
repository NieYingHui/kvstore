#ifndef _MM_POOL_
#define _MM_POOL_

#include <stdint.h>
#include <stddef.h>


typedef struct mp_pool_s mp_pool_s;

struct mp_pool_s *mp_create_pool(size_t size);
void mp_destory_pool(struct mp_pool_s *pool);
void *mp_alloc(struct mp_pool_s *pool, size_t size);
void *mp_nalloc(struct mp_pool_s *pool, size_t size);
void *mp_calloc(struct mp_pool_s *pool, size_t size);
void mp_free(struct mp_pool_s *pool, void *p);

// struct mp_pool_s *mp_create_pool(size_t size);
// void mp_destory_pool(struct mp_pool_s *pool);
// void mp_reset_pool(struct mp_pool_s *pool);
// void *mp_alloc(struct mp_pool_s *pool, size_t size);
// void *mp_nalloc(struct mp_pool_s *pool, size_t size);
// void *mp_calloc(struct mp_pool_s *pool, size_t size);
// void mp_free(struct mp_pool_s *pool, void *p);
// void mp_free_small(struct mp_pool_s *pool, void *p);
// void mp_stat(struct mp_pool_s *pool);


#endif // _MM_POOL_