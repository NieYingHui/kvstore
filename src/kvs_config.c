#include "kvs_config.h"
#include "network_backend.h"
#include "../kvstore.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>

static void kvs_copy_str(char *dst, size_t dstsz, const char *src) { // 字符串复制函数
    if (!dst || dstsz == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    (void)snprintf(dst, dstsz, "%s", src);
}

static int kvs_path_is_relative_filename(const char *s) { // 判断给定的字符串是否是一个相对文件名（而不是路径）
    if (!s || s[0] == '\0') return 0;
    /* 将包含 '/' 的任何内容视为路径 */
    if (strchr(s, '/') != NULL) return 0; // 包含路径分隔符的完整或相对路径（如 "dir/file.txt" 或 "../file.txt"）
    return 1; // 表示输入是一个简单的文件名（如 "file.txt"）
}

// 将目录路径和文件名组合成一个完整的文件路径
static void kvs_prefix_dir(char *dst, size_t dstsz, const char *dir, const char *file) {
// char dst[256];  // 目标缓冲区，足够大
// const char *dir = "/home/user";  // 目录路径
// const char *file = "data.txt";  // 文件名

    if (!dst || dstsz == 0) return;
    if (!file) {
        dst[0] = '\0';
        return;
    }
    if (!dir || dir[0] == '\0' || !kvs_path_is_relative_filename(file)) {
        (void)snprintf(dst, dstsz, "%s", file);
        return;
    }
    (void)snprintf(dst, dstsz, "%s/%s", dir, file); // dst = "/home/user/data.txt"
}

void kvs_server_config_init(kvs_server_config_t *cfg) {
    if (!cfg) return;

    cfg->port = 2000;

    cfg->enable_persistence = PERSISTENCE_ENABLE;

    /* Redis 默认：appendonly 关闭，RDB 打开（由 save 规则控制） */
    cfg->enable_aof         = 0;
    cfg->aof_fsync          = KVS_AOF_FSYNC_EVERYSEC;

    memset(cfg->dir, 0, sizeof(cfg->dir));
    memset(cfg->aof_path, 0, sizeof(cfg->aof_path));
    memset(cfg->rdb_path, 0, sizeof(cfg->rdb_path));

    kvs_copy_str(cfg->dir, sizeof(cfg->dir), "log");
    /* Redis 默认文件名：dump.rdb / appendonly.aof（这里允许用户写相对路径） */
    kvs_prefix_dir(cfg->rdb_path, sizeof(cfg->rdb_path), cfg->dir, "dump.rdb");
    kvs_prefix_dir(cfg->aof_path, sizeof(cfg->aof_path), cfg->dir, "appendonly.aof");

    /* Redis 默认 save 规则：900 1; 300 10; 60 10000 */
    cfg->save_rules_count = 3;
    cfg->save_rules[0].seconds = 900;
    cfg->save_rules[0].changes = 1;
    cfg->save_rules[1].seconds = 300;
    cfg->save_rules[1].changes = 10;
    cfg->save_rules[2].seconds = 60;
    cfg->save_rules[2].changes = 10000;

    cfg->network_backend = NETWORK_SELECT;
}

static char *trim_leading(char *s) {  // 去除字符串开头的空白字符
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

static void trim_trailing(char *s) { // 去除字符串末尾空白字符
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
}

// 加载配置
int kvs_load_config(const char *path, kvs_server_config_t *cfg,
                    char *err, size_t errlen) {
    if (!path || !cfg) {
        if (err && errlen > 0) {
            snprintf(err, errlen, "invalid arguments");
        }
        return -1;
    }

    FILE *fp = fopen(path, "r");
    if (!fp) {
        if (err && errlen > 0) {
            snprintf(err, errlen, "cannot open %s: %s", path, strerror(errno));
        }
        return -1;
    }

    char line[256];
    int lineno = 0;

    while (fgets(line, sizeof(line), fp) != NULL) {
        lineno++;
        char *p = trim_leading(line);
        trim_trailing(p);

        if (*p == '\0' || *p == '#') {
            continue; /* 空行或注释 */
        }

        char key[64] = {0};
        char val1[64] = {0};
        char val2[64] = {0};
        int nfields = sscanf(p, "%63s %63s %63s", key, val1, val2);
        if (nfields < 2) {
            continue;
        }

        if (strcmp(key, "port") == 0) {
            char *endptr = NULL;
            long port = strtol(val1, &endptr, 10);
            if (endptr == val1 || *endptr != '\0' || port <= 0 || port > 65535) {
                if (err && errlen > 0) {
                    snprintf(err, errlen,
                             "invalid port '%s' at line %d", val1, lineno);
                }
                fclose(fp);
                return -1;
            }
            cfg->port = (unsigned short)port;
        } else if (strcmp(key, "dir") == 0) {
            memset(cfg->dir, 0, sizeof(cfg->dir));
            kvs_copy_str(cfg->dir, sizeof(cfg->dir), val1);
        } else if (strcmp(key, "enable_persistence") == 0) {
            cfg->enable_persistence = (strcmp(val1, "1") == 0 || strcasecmp(val1, "yes") == 0) ? 1 : 0;
        } else if (strcmp(key, "appendonly") == 0) {
            cfg->enable_aof = (strcmp(val1, "1") == 0 || strcasecmp(val1, "yes") == 0) ? 1 : 0;
        } else if (strcmp(key, "appendfilename") == 0) {
            memset(cfg->aof_path, 0, sizeof(cfg->aof_path));
            kvs_copy_str(cfg->aof_path, sizeof(cfg->aof_path), val1);
        } else if (strcmp(key, "appendfsync") == 0) {
            if (strcasecmp(val1, "always") == 0) cfg->aof_fsync = KVS_AOF_FSYNC_ALWAYS;
            else if (strcasecmp(val1, "everysec") == 0) cfg->aof_fsync = KVS_AOF_FSYNC_EVERYSEC;
            else if (strcasecmp(val1, "no") == 0) cfg->aof_fsync = KVS_AOF_FSYNC_NO;
            else {
                if (err && errlen > 0) {
                    snprintf(err, errlen,
                             "invalid appendfsync '%s' at line %d", val1, lineno);
                }
                fclose(fp);
                return -1;
            }

        } else if (strcmp(key, "dbfilename") == 0) {
            memset(cfg->rdb_path, 0, sizeof(cfg->rdb_path));
            kvs_copy_str(cfg->rdb_path, sizeof(cfg->rdb_path), val1);

        } else if (strcmp(key, "save") == 0) {
            /* 兼容 Redis：save "" 用于禁用 RDB 自动保存 */
            if (strcmp(val1, "\"\"") == 0) {
                cfg->save_rules_count = 0;
                continue;
            }
            if (nfields < 3) {
                if (err && errlen > 0) {
                    snprintf(err, errlen,
                             "save expects '<seconds> <changes>' at line %d", lineno);
                }
                fclose(fp);
                return -1;
            }
            char *endptr = NULL;
            long seconds = strtol(val1, &endptr, 10);
            if (endptr == val1 || *endptr != '\0' || seconds < 0) {
                if (err && errlen > 0) {
                    snprintf(err, errlen,
                             "invalid save seconds '%s' at line %d", val1, lineno);
                }
                fclose(fp);
                return -1;
            }
            endptr = NULL;
            long changes = strtol(val2, &endptr, 10);
            if (endptr == val2 || *endptr != '\0' || changes < 0) {
                if (err && errlen > 0) {
                    snprintf(err, errlen,
                             "invalid save changes '%s' at line %d", val2, lineno);
                }
                fclose(fp);
                return -1;
            }
            if (cfg->save_rules_count < KVS_MAX_SAVE_RULES) {
                cfg->save_rules[cfg->save_rules_count].seconds = (int)seconds;
                cfg->save_rules[cfg->save_rules_count].changes = (int)changes;
                cfg->save_rules_count++;
            }
        } else if (strcmp(key, "network_backend") == 0) {
            int backend = -1;
            int all_digits = 1;
            for (const char *p = val1; *p; ++p) {
                if (*p < '0' || *p > '9') {
                    all_digits = 0;
                    break;
                }
            }

            if (all_digits) {
                char *endptr = NULL;
                long v = strtol(val1, &endptr, 10);
                if (endptr == val1 || *endptr != '\0') {
                    if (err && errlen > 0) {
                        snprintf(err, errlen,
                                 "invalid network_backend '%s' at line %d", val1, lineno);
                    }
                    fclose(fp);
                    return -1;
                }
                backend = (int)v;
            } else {
                if (strcasecmp(val1, "reactor") == 0) {
                    backend = NETWORK_REACTOR;
                } else if (strcasecmp(val1, "proactor") == 0) {
                    backend = NETWORK_PROACTOR;
                } else if (strcasecmp(val1, "ntyco") == 0 || strcasecmp(val1, "co") == 0) {
                    backend = NETWORK_NTYCO;
                } else {
                    if (err && errlen > 0) {
                        snprintf(err, errlen,
                                 "unknown network_backend '%s' at line %d", val1, lineno);
                    }
                    fclose(fp);
                    return -1;
                }
            }

            if (backend < NETWORK_REACTOR || backend > NETWORK_NTYCO) {
                if (err && errlen > 0) {
                    snprintf(err, errlen,
                             "invalid network_backend value '%s' at line %d", val1, lineno);
                }
                fclose(fp);
                return -1;
            }
            cfg->network_backend = backend;
        } else {
        }
    }

    /*
     * 解析结束后再做一次路径归一化：支持 dir 放在后面写。
     * 规则：当 aof_path/rdb_path 是“纯文件名”时，自动前缀 dir。
     */
    if (cfg->dir[0] != '\0') {
        if (kvs_path_is_relative_filename(cfg->aof_path)) {
            char tmp[sizeof(cfg->aof_path)];
            kvs_copy_str(tmp, sizeof(tmp), cfg->aof_path);
            kvs_prefix_dir(cfg->aof_path, sizeof(cfg->aof_path), cfg->dir, tmp);
        }
        if (kvs_path_is_relative_filename(cfg->rdb_path)) {
            char tmp[sizeof(cfg->rdb_path)];
            kvs_copy_str(tmp, sizeof(tmp), cfg->rdb_path);
            kvs_prefix_dir(cfg->rdb_path, sizeof(cfg->rdb_path), cfg->dir, tmp);
        }
    }
    
    fclose(fp);
    return 0;
}
