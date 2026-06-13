#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <stdio.h>
#include <sys/time.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <sys/wait.h>


#include "kvs_rdb.h"
#include "kvs_aof.h"

#include "../../kvstore.h"
#include "../engine.h"

/*
 * 保存策略
 * - RDB: 按 save 规则触发保存到 dbfilename
 * - AOF: 每次写命令追加 RESP 命令流
 */
static const char *log_dir = NULL; /* 对应配置项 dir */
static const char *rdb_file = NULL; /* 对应配置项 dbfilename（这里保存为完整路径） */
static char rdb_tmp_file[512] = {0}; /* 后台 BGSAVE 写入的临时文件路径 */

static long lastsave_time_ms = 0; /* 上次成功保存 RDB 的时间（毫秒） */
static int dirty = 0;            /* 自上次保存以来的写入次数 */

/*
 * BGSAVE 状态：
 * - fork 子进程写入临时文件 rdb_tmp_file
 * - 子进程完成后 rename 原子替换 rdb_file
 * - 父进程通过 poll(waitpid WNOHANG) 采集结果，更新 lastsave/dirty
 */
static pid_t bgsave_child_pid = -1;
static int dirty_at_bgsave_start = 0;


static int ensure_log_dir(void);
static void kvs_persistence_load_config(void);

static long now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000L + tv.tv_usec / 1000L;
}

// 后台保存（bgsave）的轮询函数
static void kvs_persistence_bgsave_poll(void) {
    if (bgsave_child_pid <= 0) return;

    int status = 0;
    pid_t r = waitpid(bgsave_child_pid, &status, WNOHANG); // 非阻塞方式（WNOHANG）检查子进程状态
    if (r == 0) return; /* 仍在运行 */
    if (r < 0) {
        /* waitpid 错误：保守地标记为未运行 */
        bgsave_child_pid = -1;
        return;
    }

    /* 子进程退出 */
    bgsave_child_pid = -1;

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        long finish_ms = now_ms();
        lastsave_time_ms = finish_ms;

        /*
         * dirty 语义：自上次成功保存以来发生的写入次数。
         * BGSAVE 期间 dirty 仍在增长；保存完成后只扣除 fork 时刻之前的那部分。
         */
        if (dirty >= dirty_at_bgsave_start) dirty -= dirty_at_bgsave_start;
        else dirty = 0;
    }
}

// 启动后台保存（background save）操作，将内存中的数据持久化到磁盘
static int kvs_persistence_bgsave_start(void) {
    const kvs_server_config_t *cfg = &kvs_server.config;
    if (!cfg->enable_persistence) return 0;

    if (!rdb_file) {
        kvs_persistence_load_config();
    }
    (void)ensure_log_dir();

    /* 正在运行 */
    if (bgsave_child_pid > 0) return 1;

    if (rdb_tmp_file[0] == '\0' && rdb_file) {
        (void)snprintf(rdb_tmp_file, sizeof(rdb_tmp_file), "%s.tmp", rdb_file);
    }
    if (!rdb_file || rdb_file[0] == '\0' || rdb_tmp_file[0] == '\0') return -1;

    /* 捕获点内脏数据 */
    dirty_at_bgsave_start = dirty;

    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }

    if (pid == 0) {
        /* child */
        int ok = 0;
        if (kvs_rdb_save_to_file(rdb_tmp_file) != 0) ok = -1;
        if (ok == 0) {
            if (rename(rdb_tmp_file, rdb_file) != 0) ok = -1;
        }
        _exit(ok == 0 ? 0 : 1);
    }

    /* parent */
    bgsave_child_pid = pid;
    return 0;
}

/*
@brief: ensure log directory exists
@param: void
@return: 0:succse -1:faild
*/
static int ensure_log_dir(void) {
    struct stat st = {0};
    if(stat(log_dir, &st) == -1) {
        if(mkdir(log_dir, 0755) != 0) {
            perror("mkdir log");
            return -1;
        }
    }
    return 0;
}

/* 从全局 kvs_server.config 中加载持久化相关配置。 */
static void kvs_persistence_load_config(void) {
    const kvs_server_config_t *cfg = &kvs_server.config;

    /* 我们在 config 中已经做了相对路径归一化 */
    log_dir = (cfg->dir[0] != '\0') ? cfg->dir : "log";
    rdb_file = (cfg->rdb_path[0] != '\0') ? cfg->rdb_path : "log/dump.rdb";

    /* 生成默认临时文件路径：用于后台 BGSAVE */
    (void)snprintf(rdb_tmp_file, sizeof(rdb_tmp_file), "%s.tmp", rdb_file);
}


static int kvs_persistence_write_snapshot_file(const char *path) {
    if (!path || path[0] == '\0') return -1;

    if (!log_dir || !rdb_file) {
        kvs_persistence_load_config();
    }

    (void)ensure_log_dir();

    if (kvs_rdb_save_to_file(path) != 0) {
        perror("write rdb(snapshot) failed");
        return -1;
    }
    return 0;
}


int kvs_persistence_rdb_save_to_file(const char *path) {
    return kvs_persistence_write_snapshot_file(path);
}


int kvs_persistence_init(void) {
    const kvs_server_config_t *cfg = &kvs_server.config;

    /* 如果整体关闭持久化，则直接返回成功，不做任何初始化。 */
    if (!cfg->enable_persistence) {
        return 0;
    }

    kvs_persistence_load_config();

    if (ensure_log_dir() != 0) return -1;

    /* 只初始化 AOF；RDB 保存由 save 规则触发。 */

    /* AOF init（与 RDB 保存解耦）。 */
    if (cfg->enable_aof) {
        if (kvs_aof_init() != 0) {
            fprintf(stderr, "kvs_aof_init failed\n");
            return -1;
        }
    }

    lastsave_time_ms = now_ms();
    dirty = 0;

    return 0;
}

// 在写入命令后触发数据持久化操作
int kvs_persistence_on_write_cmdline(const char *cmdline) {
    (void)cmdline;
    const kvs_server_config_t *cfg = &kvs_server.config;
    if (!cfg->enable_persistence) return 0;

    /* 该函数在“写命令成功后”被调用：用于驱动 save 规则（后台 BGSAVE）。 */
    dirty++;

    /* 非阻塞采集 bgsave 结果 */
    kvs_persistence_bgsave_poll();

    if (cfg->save_rules_count <= 0) {
        return 0; /* 等价于 Redis: save "" */
    }

    struct timeval tv;
    gettimeofday(&tv, NULL);
    long now = tv.tv_sec * 1000L + tv.tv_usec / 1000L;
    long elapsed_sec = (now - lastsave_time_ms) / 1000L; // 已过去的时间。

    for (int i = 0; i < cfg->save_rules_count; ++i) {
        int seconds = cfg->save_rules[i].seconds; // 秒数
        int changes = cfg->save_rules[i].changes; // 数据变更次数
        if (seconds <= 0 || changes <= 0) continue;

        if (dirty >= changes && elapsed_sec >= seconds) {
            /*
             * 触发后台 BGSAVE。
             * 如果已经在进行中，则保持 dirty 计数，等下次触发/完成后再评估。
             */
            (void)kvs_persistence_bgsave_start();
            break;
        }
    }

    return 0;
}

int kvs_persistence_on_write_argv(int argc, const char * const *argv) {
    (void)argc;
    (void)argv;
    /* argv 版本在协议层被频繁调用，直接复用同一套 dirty/save 逻辑即可。 */
    return kvs_persistence_on_write_cmdline(NULL);
}

int kvs_persistence_rdb_load_from_file(const char *path) {
    return kvs_rdb_load_from_file(path);
}


int kvs_persistence_recover(void) {
    const kvs_server_config_t *cfg = &kvs_server.config;
    if (!cfg->enable_persistence) {
        return 0;
    }

    if (!rdb_file) {
        kvs_persistence_load_config();
    }

    /* 启动恢复顺序：先尝试加载 RDB，再回放 AOF（如果启用）。 */
    (void)kvs_rdb_load_from_file(rdb_file);
    if (cfg->enable_aof) {
        return kvs_aof_recover();
    }
    return 0;

}

int kvs_persistence_bgsave(void) {
    const kvs_server_config_t *cfg = &kvs_server.config;
    if (!cfg->enable_persistence) return 0;

    /* 手动触发：非阻塞启动；返回 0=已启动, 1=忙, -1=失败 */
    kvs_persistence_bgsave_poll();
    return kvs_persistence_bgsave_start();
}


void kvs_persistence_close(void) {
    /* 进程退出前尽量等待后台 bgsave 收尾，避免生成半文件。 */
    if (bgsave_child_pid > 0) {
        int status = 0;
        (void)waitpid(bgsave_child_pid, &status, 0);
        bgsave_child_pid = -1;
        kvs_persistence_bgsave_poll(); // 进行轮询检查，确保所有持久化操作已完成。
    }
    kvs_aof_close();
}

