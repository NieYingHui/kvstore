#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>

#include "engine.h"
#include "persistence/kvs_persistence.h"
#include "memory/mm_pool.h"


static pthread_rwlock_t g_db_lock = PTHREAD_RWLOCK_INITIALIZER;
#if MM_POOL_ENABLE
static mp_pool_s *g_mm_pool = NULL;
#endif

/* 全局服务器状态实例，所有的 global_* 引用都通过宏映射到这里。 */
kvsServer kvs_server = {0};

void *kvs_malloc(size_t size) {
    if (size == 0) return NULL;
#if MM_POOL_ENABLE
    if (g_mm_pool != NULL) {
        void *p = mp_calloc(g_mm_pool, size);
        if (p) return p;
    }
#endif
    return calloc(1, size);
}

void kvs_free(void *ptr) {
    if (!ptr) return;
#if MM_POOL_ENABLE
    if (g_mm_pool != NULL) {
        mp_free(g_mm_pool, ptr);
        return;
    }
#endif
    free(ptr);
}

// 获取读锁（共享锁）
void kvs_db_rdlock(void) {
    (void)pthread_rwlock_rdlock(&g_db_lock);
}

// 获取写锁（排他锁）
void kvs_db_wrlock(void) {
    (void)pthread_rwlock_wrlock(&g_db_lock);
}

// 释放锁
void kvs_db_unlock(void) {
    (void)pthread_rwlock_unlock(&g_db_lock);
}

int init_kvengine(void) {
#if MM_POOL_ENABLE
    if (!g_mm_pool) {
        g_mm_pool = mp_create_pool(1 << 27); /* 128MB pool */
        if (!g_mm_pool) {
            fprintf(stderr, "Failed to create memory pool, fallback to malloc/free.\n");
        }
    }
#endif

#if ENABLE_ARRAY
    if (kvs_array_create(&global_array) != 0) {
        fprintf(stderr, "kvs_array_create failed\n");
        return -1;
    }
#endif

#if ENABLE_HASH
    if (kvs_hash_create(&global_hash) != 0) {
        fprintf(stderr, "kvs_hash_create failed\n");
        return -1;
    }
#endif

#if ENABLE_RBTREE
    if (kvs_rbtree_create(&global_rbtree) != 0) {
        fprintf(stderr, "kvs_rbtree_create failed\n");
        return -1;
    }
#endif

#if ENABLE_SKIPLIST
    if (kvs_skiplist_create(&global_skiplist) != 0) {
        fprintf(stderr, "kvs_skiplist_create failed\n");
        return -1;
    }
#endif

#if PERSISTENCE_ENABLE
    if (kvs_persistence_init() != 0) {
        fprintf(stderr, "kvs_persistence_init failed\n");
        return -1;
    }
    if (kvs_persistence_recover() != 0) {
        fprintf(stderr, "kvs_persistence_recover failed\n");
        return -1;
    }
#endif

    return 0;
}

void destroy_kvengine(void) {
#if PERSISTENCE_ENABLE
    (void)kvs_persistence_bgsave();
    kvs_persistence_close();
#endif

#if ENABLE_ARRAY
    kvs_array_destroy(&global_array);
#endif

#if ENABLE_HASH
    kvs_hash_destory(&global_hash);
#endif

#if ENABLE_RBTREE
    kvs_rbtree_destory(&global_rbtree);
#endif

#if ENABLE_SKIPLIST
    kvs_skiplist_destory(&global_skiplist);
#endif

#if MM_POOL_ENABLE
    if (g_mm_pool) {
        mp_destory_pool(g_mm_pool);
        g_mm_pool = NULL;
    }
#endif
}
