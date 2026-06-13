#ifndef KVS_ENGINE_H
#define KVS_ENGINE_H

#include <stdlib.h>

#include "kvs_config.h"
#include "../kvstore.h"

/*
 * kvsServer 负责集中保存服务端的全局状态，
 */
typedef struct kvsServer {
	kvs_server_config_t config;   /* 运行时配置 */
	unsigned short       port;    /* 当前监听端口*/

	kvs_array_t   array;          /* 简单数组存储 */
	kvs_rbtree_t  rbtree;         /* 红黑树存储 */
	kvs_hash_t    hash;           /* 哈希表存储 */
	kvs_skiplist_t skiplist;      /* 跳表存储 */
} kvsServer;

/* 全局唯一的服务器状态实例。 */
extern kvsServer kvs_server;

#define global_array    (kvs_server.array)
#define global_rbtree   (kvs_server.rbtree)
#define global_hash     (kvs_server.hash)
#define global_skiplist (kvs_server.skiplist)

int init_kvengine(void);
void destroy_kvengine(void);

void *kvs_malloc(size_t size);
void kvs_free(void *ptr);

void kvs_db_rdlock(void);
void kvs_db_wrlock(void);
void kvs_db_unlock(void);

#endif
