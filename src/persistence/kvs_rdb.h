#ifndef _KVS_RDB_H_
#define _KVS_RDB_H_

/*
 * Redis RDB 模块（对齐 Redis RDB 文件格式）。
 * 当前实现为最小子集：仅支持 string key/value。
 */

int kvs_rdb_save(void);
int kvs_rdb_load(void);

/* Explicit path variants. */
int kvs_rdb_save_to_file(const char *path);
int kvs_rdb_load_from_file(const char *path);

#endif
