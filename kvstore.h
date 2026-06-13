

// 定义我们的协议protocol 五种命令
// SET Key Value
// GET Key
// DEL Key
// MOD Key Value
// EXIST Key

#ifndef _KVSTORE_H__
#define _KVSTORE_H__


#define KVS_MAX_TOKENS         1024  

#define ENABLE_ARRAY           1
#define ENABLE_RBTREE          1
#define ENABLE_HASH            1
#define ENABLE_SKIPLIST        1 


#define PERSISTENCE_ENABLE		1

/*
 * MM_POOL_ENABLE:
 * - 0: kvs_malloc/kvs_free -> calloc/free（可通过 LD_PRELOAD 或链接 jemalloc 替换底层分配器）
 * - 1: kvs_malloc/kvs_free -> 内存池（mm_pool.c），失败时回退到 calloc/free
 *
 * 为了便于 benchmark，对该宏允许通过编译参数覆盖：
 *   make all EXTRA_CFLAGS='-DMM_POOL_ENABLE=1'
 */
#ifndef MM_POOL_ENABLE
#define MM_POOL_ENABLE			0
#endif

#include "client.h"
#include <stddef.h>

void *kvs_malloc(size_t size);
void kvs_free(void *ptr);
int kvs_spilt_token(char *msg, char *tokens[]);
int kvs_protocol_filter(char **tokens, int count, char *response);
int kvs_protocol(char *msg, int length, char *response);
int init_kvengine(void);
void destroy_kvengine(void);
int kvs_server_run(unsigned short port);



#if ENABLE_ARRAY

typedef struct kvs_array_item_s {
    char *key;
    char *value;
} kvs_array_item_t;

#define KVS_ARRAY_SIZE       1024

typedef struct kvs_array_s {
    kvs_array_item_t *table;
    int idx;
    int total;
} kvs_array_t;


int kvs_array_create(kvs_array_t *inst);
void kvs_array_destroy(kvs_array_t *inst);
int kvs_array_set(kvs_array_t *inst, char *key, char *value);
char* kvs_array_get(kvs_array_t *inst, char *key);
int kvs_array_del(kvs_array_t *inst, char *key);
int kvs_array_mod(kvs_array_t *inst, char *key, char *value);
int kvs_array_exist(kvs_array_t *inst, char *key);

#endif


#if ENABLE_RBTREE


#define ENABLE_KEY_CHAR 1

#define RED				1
#define BLACK 			2

#if ENABLE_KEY_CHAR

typedef char* KEY_TYPE;
#else
typedef int KEY_TYPE;

#endif


typedef struct _rbtree_node {
	unsigned char color;
	struct _rbtree_node *right;
	struct _rbtree_node *left;
	struct _rbtree_node *parent;
	KEY_TYPE key;
	void *value;
} rbtree_node;

typedef struct _rbtree {
	rbtree_node *root;
	rbtree_node *nil;
} rbtree;

typedef struct _rbtree kvs_rbtree_t;

int kvs_rbtree_create(kvs_rbtree_t *inst);
void kvs_rbtree_destory(kvs_rbtree_t *inst);
int kvs_rbtree_set(kvs_rbtree_t *inst, char *key, char *value);
char* kvs_rbtree_get(kvs_rbtree_t *inst, char *key);
int kvs_rbtree_del(kvs_rbtree_t *inst, char *key);
int kvs_rbtree_mod(kvs_rbtree_t *inst, char *key, char *value);
int kvs_rbtree_exist(kvs_rbtree_t *inst, char *key);



#endif


#if ENABLE_HASH

#define MAX_KEY_LEN	128
#define MAX_VALUE_LEN	512
#define MAX_TABLE_SIZE	1024

#define ENABLE_KEY_POINTER	1


typedef struct hashnode_s {
#if ENABLE_KEY_POINTER
	char *key;
	char *value;
#else
	char key[MAX_KEY_LEN];
	char value[MAX_VALUE_LEN];
#endif
	struct hashnode_s *next;
	
} hashnode_t;


typedef struct hashtable_s {

	hashnode_t **nodes; //* change **, 

	int max_slots;
	int count;

} hashtable_t;

typedef struct hashtable_s kvs_hash_t;

hashnode_t *_create_node(char *key, char *value);
int kvs_hash_create(kvs_hash_t *hash);
void kvs_hash_destory(kvs_hash_t *hash);
int kvs_hash_set(kvs_hash_t *hash, char *key, char *value);
char * kvs_hash_get(kvs_hash_t *hash, char *key);
int kvs_hash_mod(kvs_hash_t *hash, char *key, char *value);
int kvs_hash_count(kvs_hash_t *hash);
int kvs_hash_del(kvs_hash_t *hash, char *key);
int kvs_hash_exist(kvs_hash_t *hash, char *key);


#endif

#if ENABLE_SKIPLIST

#define MAX_LEVEL 6  // 定义最大层级

#define ENABLE_KEY_SKIPLIST 1

typedef struct Node {
#if ENABLE_KEY_SKIPLIST
	char *key;  // 键值
	char *value;  // 存储的值

#else
    int key;  // 键值
    int value;  // 存储的值
#endif
    struct Node** forward;  // 指向各层下一个节点的指针数组
} Node;

typedef struct SkipList {
    int level;  // 当前跳表的最大层级
    Node* header;  // 头节点
} SkipList;

typedef struct SkipList kvs_skiplist_t;


Node* createNode(int level, char *key, char *value);
int kvs_skiplist_create(SkipList* skipList);
int randomLevel();
int kvs_skiplist_set(SkipList* skipList, char *key, char *value);
int kvs_skiplist_mod(SkipList* skipList, char *key, char *value);
void display(SkipList* skipList);
void kvs_skiplist_destory(SkipList* skipList);
char* kvs_skiplist_get(SkipList* skipList, char *key);
int kvs_skiplist_del(SkipList* skipList, char *key);
int kvs_skiplist_exist(SkipList* skipList, char *key);


#endif


#endif


