#ifndef KVS_CONFIG_H
#define KVS_CONFIG_H

#include <stddef.h>

#define KVS_MAX_PATH_LEN 256

#define KVS_MAX_SAVE_RULES 16

/*
 * Redis 风格 RDB 触发规则：save <seconds> <changes>
 * - seconds: 时间窗口（秒）
 * - changes: 在窗口内至少发生多少次写入（dirty 计数）
 */
typedef struct kvs_save_rule_s {
    int seconds;
    int changes;
} kvs_save_rule_t;

/*
 * 服务器配置结构（Redis 风格的持久化配置）：
 * - AOF: appendonly / appendfilename / appendfsync
 * - RDB: dir / dbfilename / save
 */
typedef struct kvs_server_config_s {
    unsigned short port;

    int enable_persistence;        /* 整体持久化开关 */

    int enable_aof;                /* appendonly: yes/no */

    /* AOF fsync policy */
    int aof_fsync;                 /* KVS_AOF_FSYNC_* */

    char dir[KVS_MAX_PATH_LEN];          /* Redis: dir */
    char aof_path[KVS_MAX_PATH_LEN];     /* Redis: appendfilename（可写相对/绝对路径） */
    char rdb_path[KVS_MAX_PATH_LEN];     /* Redis: dbfilename（可写相对/绝对路径） */
    kvs_save_rule_t save_rules[KVS_MAX_SAVE_RULES];
    int save_rules_count; // save规则条数

    int network_backend;           /* 网络后端选择：NETWORK_REACTOR/PROACTOR/NTYCO */
} kvs_server_config_t;

/* AOF fsync policy values (modeled after Redis semantics). */
#define KVS_AOF_FSYNC_NO        0
#define KVS_AOF_FSYNC_ALWAYS    1
#define KVS_AOF_FSYNC_EVERYSEC  2

void kvs_server_config_init(kvs_server_config_t *cfg);


int kvs_load_config(const char *path, kvs_server_config_t *cfg,
                    char *err, size_t errlen);

#endif
