#include "kvs_aof.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <stdint.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/wait.h>

#include "../../kvstore.h"
#include "../engine.h"
#include "../ebpf/kvs_ebpf_hook.h"
#include "../resp.h"

static int g_aof_fd = -1;
static int g_aof_fsync_policy = 0; /* KVS_AOF_FSYNC_* */
static long g_last_fsync_ms = 0;

/*
 * BGREWRITEAOF（后台重写）状态：
 * - 子进程：fork 后把“当前内存快照”写入 g_tmp_path
 * - 父进程：继续对外服务，写命令仍追加到旧 AOF，同时把增量命令写入 g_incr_path
 * - 合并线程：等待子进程结束后，把增量文件 merge 到 tmp，rename 原子替换，然后 reopen AOF
 */
static pthread_mutex_t g_rewrite_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_incr_fd = -1;
static pid_t g_rewrite_child_pid = -1;
static pthread_t g_rewrite_thread;
static int g_rewrite_thread_started = 0;
static char g_aof_path[512] = {0};
static char g_tmp_path[512] = {0};
static char g_incr_path[512] = {0};

// 获取当前时间（毫秒级）
static long now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000L + tv.tv_usec / 1000L;
}

// 确保日志目录存在
static int ensure_log_dir_exists(void) {
    const char *log_dir = (kvs_server.config.dir[0] != '\0') // 是否为空字符串
                            ? kvs_server.config.dir
                            : "log";
    struct stat st = {0};
    if (stat(log_dir, &st) == -1) {
        if (mkdir(log_dir, 0755) != 0) {
            return -1;
        }
    }
    return 0;
}

// 将数据完整写入到指定的文件描述符
static int write_all(int fd, const char *data, size_t len) {
    if (fd < 0 || !data || len == 0) return -1;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

// 计算无符号64位整数(uint64_t)位数
static size_t u64_digits(uint64_t v) {
    size_t n = 1;
    while (v >= 10) {
        v /= 10;
        n++;
    }
    return n;
}

/*
 * 将 argv 中的 RESP 数组序列化为单个连续缓冲区并一次性写入。
 * 此举可避免 kvs_resp_append_array_argv 引发的多次小规模 write() 系统调用。
 *
 * Returns:
 *  - number of bytes written on success
 *  - -1 on error
 */
static int aof_write_argv_once(int fd, int argc, const char * const *argv) {
    if (fd < 0 || argc <= 0 || !argv) return -1;
// int argc = 3;
// const char *argv[] = {"SET", "key", "value"};

    /* 计算总字节数: *<argc>\r\n + sum($<len>\r\n<arg>\r\n). */
    uint64_t total = 0;
    total += 1 + u64_digits((uint64_t)argc) + 2; // 1 (星号) + 1 (数字3的位数) + 2 (\r\n) = 4字节

    for (int i = 0; i < argc; i++) { // $3\r\nSET\r\n
        const char *s = argv[i] ? argv[i] : "";
        size_t slen = strlen(s);
        total += 1 + u64_digits((uint64_t)slen) + 2;
        total += (uint64_t)slen;
        total += 2;
    }

    if (total == 0 || total > (uint64_t)SIZE_MAX) return -1;

    char *buf = (char *)kvs_malloc((size_t)total);
    if (!buf) return -1;

    size_t off = 0;
    int n = snprintf(buf + off, (size_t)total - off, "*%d\r\n", argc); // *3\r\n
    if (n <= 0) {
        kvs_free(buf);
        return -1;
    }
    off += (size_t)n;

    for (int i = 0; i < argc; i++) {
        const char *s = argv[i] ? argv[i] : "";
        size_t slen = strlen(s);

        n = snprintf(buf + off, (size_t)total - off, "$%zu\r\n", slen); // $3\r\n
        if (n <= 0) {
            kvs_free(buf);
            return -1;
        }
        off += (size_t)n;

        if (slen > 0) {
            memcpy(buf + off, s, slen); // 复制：SET
            off += slen;
        }
        buf[off++] = '\r'; // 添加：\r\n
        buf[off++] = '\n';
    }

    if (off != (size_t)total) {
        kvs_free(buf);
        return -1;
    }

    int rc = write_all(fd, buf, (size_t)total);
    kvs_free(buf);
    if (rc != 0) return -1;
    if (total > (uint64_t)INT_MAX) return -1;
    return (int)total;
}

// 追加数据到文件中
static int aof_append_cb(const char *data, int len, void *ud) {
    int fd = ud ? *(int *)ud : -1; // 用户自定义数据指针，这里被用作文件描述符
    if (len <= 0) return 0;
    if (write_all(fd, data, (size_t)len) != 0) return -1;
    return len;
}

static int aof_maybe_fsync(void) {
    if (g_aof_fd < 0) return 0;
    if (g_aof_fsync_policy == KVS_AOF_FSYNC_NO) {
        return 0;
    }
    if (g_aof_fsync_policy == KVS_AOF_FSYNC_ALWAYS) {
        /* Redis：始终启用appendfsync */
        return (fsync(g_aof_fd) == 0) ? 0 : -1;
    }

    /* everysec */
    long ms = now_ms();
    if (g_last_fsync_ms == 0 || (ms - g_last_fsync_ms) >= 1000) {
        if (fsync(g_aof_fd) != 0) return -1;
        g_last_fsync_ms = ms;
    }
    return 0;
}

/* ---- 最小化 RESP 数组解析器（原地解析），匹配 protocol.c 语义 ---- */
// 解析字符串中的长整型数字
static int parse_long_field(const char *p, const char *end, long *out, const char **line_end_out) {
    if (!p || !end || !out || !line_end_out) return -1;
    const char *nl = memchr(p, '\n', (size_t)(end - p));
    if (!nl) return 0;
// const char *p = "123\n";
// const char *end = p + 4;  // 指向'\n'后面的位置
// long value;
// const char *line_end;

    long sign = 1;
    const char *q = p;
    if (q < nl && *q == '-') {
        sign = -1;
        q++;
    }
    if (q >= nl || *q < '0' || *q > '9') return -1;

    long v = 0;
    while (q < nl && *q >= '0' && *q <= '9') {
        v = v * 10 + (long)(*q - '0');
        q++;
    }
    if (q < nl && *q != '\r') return -1;

    *out = v * sign;
    *line_end_out = nl;
// *out = 123 * 1 = 123
// *line_end_out = 指向'\n'的位置
    return 1;
}

static int parse_resp_array_inplace(char *buf, int length, char *tokens[], int *consumed) {
    if (!buf || !tokens || !consumed || length <= 0) return -1;
    if (buf[0] != '*') return -1;
// char *buf = "*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n";
// int length = 33;  // 整个字符串的长度
// char *tokens[KVS_MAX_TOKENS];  // 用于存储解析出的参数
// int consumed;  // 用于存储已处理的字节数

    char *end = buf + length;

    const char *p = buf + 1;
    const char *line_end = NULL;
    long argc = 0;
    int r = parse_long_field(p, end, &argc, &line_end);
    if (r == 0) return 0;
    if (r < 0) return -1;
    if (argc <= 0 || argc > KVS_MAX_TOKENS) return -1;
    p = line_end + 1;

    const char *arg_starts[KVS_MAX_TOKENS] = {0};
    long arg_lens[KVS_MAX_TOKENS] = {0};

    for (long i = 0; i < argc; ++i) {
        if (p >= end || *p != '$') return -1;
        p++;

        long blen = 0;
        r = parse_long_field(p, end, &blen, &line_end);
        if (r == 0) return 0;
        if (r < 0) return -1;
        if (blen < 0) return -1;

        p = line_end + 1;
        if (p + blen + 2 > end) return 0;
        if (p[blen] != '\r' || p[blen + 1] != '\n') return -1;

        arg_starts[i] = p; // arg_starts[0]=指向"SET"的指针
        arg_lens[i] = blen; // arg_lens[0]=3
        p = p + blen + 2;
    }

    for (long i = 0; i < argc; ++i) {
        tokens[i] = (char *)arg_starts[i]; // tokens[0]指向"SET"
        ((char *)arg_starts[i])[arg_lens[i]] = '\0'; //每个参数的末尾添加'\0'，使其成为有效的C字符串
    }

    *consumed = (int)(p - (const char *)buf); // consumed=33
    return (int)argc;
}

static void apply_argv(int argc, char **argv) {
    if (!argv || argc <= 0) return;

    /* 对 AOF 中的命令大小写保持宽容。 */
    if (argv[0]) {
        for (char *p = argv[0]; *p; ++p) {
            if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 'a' + 'A');
        }
    }

    if(strcmp(argv[0], "SET") == 0 && argc >= 3) kvs_array_set(&global_array, argv[1], argv[2]);
    else if(strcmp(argv[0], "MOD") == 0 && argc >= 3) kvs_array_mod(&global_array, argv[1], argv[2]);
    else if(strcmp(argv[0], "DEL") == 0 && argc >= 2) kvs_array_del(&global_array, argv[1]);

    else if(strcmp(argv[0], "HSET") == 0 && argc >= 3) kvs_hash_set(&global_hash, argv[1], argv[2]);
    else if(strcmp(argv[0], "HMOD") == 0 && argc >= 3) kvs_hash_mod(&global_hash, argv[1], argv[2]);
    else if(strcmp(argv[0], "HDEL") == 0 && argc >= 2) kvs_hash_del(&global_hash, argv[1]);

    else if(strcmp(argv[0], "RSET") == 0 && argc >= 3) kvs_rbtree_set(&global_rbtree, argv[1], argv[2]);
    else if(strcmp(argv[0], "RMOD") == 0 && argc >= 3) kvs_rbtree_mod(&global_rbtree, argv[1], argv[2]);
    else if(strcmp(argv[0], "RDEL") == 0 && argc >= 2) kvs_rbtree_del(&global_rbtree, argv[1]);

    else if(strcmp(argv[0], "LSET") == 0 && argc >= 3) kvs_skiplist_set(&global_skiplist, argv[1], argv[2]);
    else if(strcmp(argv[0], "LMOD") == 0 && argc >= 3) kvs_skiplist_mod(&global_skiplist, argv[1], argv[2]);
    else if(strcmp(argv[0], "LDEL") == 0 && argc >= 2) kvs_skiplist_del(&global_skiplist, argv[1]);
}

int kvs_aof_init(void) {
    const kvs_server_config_t *cfg = &kvs_server.config;
    if (!cfg->enable_persistence || !cfg->enable_aof) {
        return 0;
    }

    const char *aof_path = (cfg->aof_path[0] != '\0') ? cfg->aof_path : "log/appendonly.aof";

    if (ensure_log_dir_exists() != 0) {
        perror("mkdir log for aof failed");
        return -1;
    }

    /* remember paths for rewrite operations */
    (void)snprintf(g_aof_path, sizeof(g_aof_path), "%s", aof_path);
    (void)snprintf(g_tmp_path, sizeof(g_tmp_path), "%s.tmp", aof_path);
    (void)snprintf(g_incr_path, sizeof(g_incr_path), "%s.incr", aof_path);
    /* clean up stale rewrite artifacts (safe to ignore errors) */
    (void)unlink(g_tmp_path); // 系统调用，用于删除文件或解除文件链接
    (void)unlink(g_incr_path);

    int fd = open(aof_path, O_CREAT | O_APPEND | O_WRONLY, 0644);
    if (fd < 0) {
        perror("open aof failed");
        return -1;
    }

    g_aof_fd = fd;
    g_aof_fsync_policy = cfg->aof_fsync;
    g_last_fsync_ms = 0;
    return 0;
}

int kvs_aof_append_argv(int argc, const char * const *argv) {
    const kvs_server_config_t *cfg = &kvs_server.config;
    if (!cfg->enable_persistence || !cfg->enable_aof) {
        return 0;
    }
    if (g_aof_fd < 0) return -1;
    if (!argv || argc <= 0) return -1;

    int fd = g_aof_fd;
    long long start_off = -1;
    {
        off_t off = lseek(fd, (off_t)0, SEEK_END);
        if (off != (off_t)-1) start_off = (long long)off;
    }

    int nbytes = aof_write_argv_once(fd, argc, argv);
    if (nbytes < 0) return -1;

    /*
     * 在AOF条目追加后，发出一个稳定的uprobe钩子。
     * The eBPF agent can read [start_off, start_off + nbytes) from the AOF file.
     */
    if (start_off >= 0 && nbytes > 0) {
        kvs_ebpf_aof_append_hook(start_off, nbytes);
    }

    /*
     * BGREWRITEAOF 期间：除了追加旧 AOF，还要把增量命令写入 .incr。
     * 后续会把 .incr 合并到 .tmp（子进程快照）里，再原子替换。
     */
    pthread_mutex_lock(&g_rewrite_mu);
    int incr_fd = g_incr_fd;
    pthread_mutex_unlock(&g_rewrite_mu);
    if (incr_fd >= 0) {
        (void)aof_write_argv_once(incr_fd, argc, argv);
    }

    if (aof_maybe_fsync() != 0) return -1;
    return 0;
}

int kvs_aof_recover(void) {
    const kvs_server_config_t *cfg = &kvs_server.config;
    if (!cfg->enable_persistence || !cfg->enable_aof) {
        return 0;
    }

    /* 启动恢复：回放 AOF（这里假设 RDB 已先加载）。 */
    const char *aof_path = (cfg->aof_path[0] != '\0') ? cfg->aof_path : "log/appendonly.aof";
    int fd = open(aof_path, O_RDONLY);
    if (fd < 0) {
        /* no aof yet is ok */
        return 0;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return 0;
    }

    void *data = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0); // 创建写时复制(COW)的映射区域
    if (data == MAP_FAILED) {
        close(fd);
        return -1;
    }

    char *p = (char *)data;
    int remaining = (int)st.st_size; // 剩余需要处理的字节数
    while (remaining > 0) {
        while (remaining > 0 && (*p == '\r' || *p == '\n' || *p == ' ' || *p == '\t')) { // 跳过空白字符
            p++;
            remaining--;
        }
        if (remaining <= 0) break;

        if (*p != '*') {
            /* 无效格式 */
            munmap(data, (size_t)st.st_size);
            close(fd);
            return -1;
        }

        char *tokens[KVS_MAX_TOKENS] = {0};
        int consumed = 0; // 记录已处理的字节数
        int argc = parse_resp_array_inplace(p, remaining, tokens, &consumed);
        if (argc <= 0 || consumed <= 0) {
            munmap(data, (size_t)st.st_size);
            close(fd);
            return -1;
        }
        apply_argv(argc, tokens);
        p += consumed;
        remaining -= consumed;
    }

    munmap(data, (size_t)st.st_size);
    close(fd);
    return 0;
}

// 将当前内存中的数据快照写入到文件描述符 fd 中
static int aof_write_snapshot_to_fd(int fd) {
    if (fd < 0) return -1;

    int out_fd = fd;
    const char *argv3[3] = {0};

#if ENABLE_ARRAY
    for (int i = 0; i < global_array.total; ++i) {
        if (!global_array.table) break;
        const char *k = global_array.table[i].key;
        const char *v = global_array.table[i].value;
        if (!k || !v) continue;
        argv3[0] = "SET";
        argv3[1] = k;
        argv3[2] = v;
        if (kvs_resp_append_array_argv(aof_append_cb, &out_fd, 3, argv3) < 0) return -1;
    }
#endif

#if ENABLE_HASH
    if (global_hash.nodes) {
        for (int i = 0; i < global_hash.max_slots; ++i) {
            hashnode_t *node = global_hash.nodes[i];
            while (node) {
#if ENABLE_KEY_POINTER
                const char *k = node->key;
                const char *v = node->value;
#else
                const char *k = node->key;
                const char *v = node->value;
#endif
                if (k && v) {
                    argv3[0] = "HSET";
                    argv3[1] = k;
                    argv3[2] = v;
                    if (kvs_resp_append_array_argv(aof_append_cb, &out_fd, 3, argv3) < 0) return -1;
                }
                node = node->next;
            }
        }
    }
#endif

#if ENABLE_RBTREE
    /* 中序遍历以生成确定性AOF */
    {
        rbtree_node *stack[2048];
        int top = 0;
        rbtree_node *cur = global_rbtree.root;
        while (cur && cur != global_rbtree.nil) {
            while (cur && cur != global_rbtree.nil) {
                if (top < (int)(sizeof(stack) / sizeof(stack[0]))) {
                    stack[top++] = cur;
                } else {
                    return -1;
                }
                cur = cur->left;
            }
            if (top == 0) break;
            cur = stack[--top];
            if (cur != global_rbtree.nil) {
#if ENABLE_KEY_CHAR
                const char *k = cur->key;
#else
                char kbuf[32];
                snprintf(kbuf, sizeof(kbuf), "%d", (int)cur->key);
                const char *k = kbuf;
#endif
                const char *v = (const char *)cur->value;
                if (k && v) {
                    argv3[0] = "RSET";
                    argv3[1] = k;
                    argv3[2] = v;
                    if (kvs_resp_append_array_argv(aof_append_cb, &out_fd, 3, argv3) < 0) return -1;
                }
            }
            cur = cur->right;
        }
    }
#endif

#if ENABLE_SKIPLIST
    if (global_skiplist.header) {
        Node *node = global_skiplist.header->forward[0];
        while (node && node->key) {
            const char *k = node->key;
            const char *v = node->value;
            if (k && v) {
                argv3[0] = "LSET";
                argv3[1] = k;
                argv3[2] = v;
                if (kvs_resp_append_array_argv(aof_append_cb, &out_fd, 3, argv3) < 0) return -1;
            }
            node = node->forward[0];
        }
    }
#endif

    return 0;
}


// 将文件内容复制到文件描述符的函数
static int copy_file_to_fd(const char *path, int out_fd) {
    if (!path || out_fd < 0) return -1;
    int in_fd = open(path, O_RDONLY);
    if (in_fd < 0) return 0; /* 缺失无妨 */

    char buf[64 * 1024]; // 64KB的缓冲区用于文件读取
    for (;;) {
        ssize_t n = read(in_fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            close(in_fd);
            return -1;
        }
        if (n == 0) break;
        if (write_all(out_fd, buf, (size_t)n) != 0) { // 读取的内容写入输出文件描述符
            close(in_fd);
            return -1;
        }
    }

    close(in_fd);
    return 0;
}

// 处理数据库重写操作的后置工作
static void *rewrite_finalize_thread(void *arg) {
    (void)arg;
    int status = 0;
    pid_t pid = -1;

    pthread_mutex_lock(&g_rewrite_mu);
    pid = g_rewrite_child_pid;
    pthread_mutex_unlock(&g_rewrite_mu);

    if (pid > 0) {
        (void)waitpid(pid, &status, 0); // 等待子进程结束
    }

    int child_ok = (pid > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0); // 子进程是否正常退出

    kvs_db_wrlock();
    pthread_mutex_lock(&g_rewrite_mu);

    /* 停止收集增量写入 */
    int incr_fd = g_incr_fd;
    g_incr_fd = -1;
    pid_t old_pid = g_rewrite_child_pid;
    g_rewrite_child_pid = -1;
    pthread_mutex_unlock(&g_rewrite_mu);

    if (incr_fd >= 0) {
        (void)fsync(incr_fd);
        close(incr_fd);
    }

    if (child_ok) {
        int tmpfd = open(g_tmp_path, O_WRONLY | O_APPEND);
        if (tmpfd >= 0) {
            int ok = 0;
            if (copy_file_to_fd(g_incr_path, tmpfd) != 0) ok = -1; // 增量文件内容复制到临时文件
            if (ok == 0 && fsync(tmpfd) != 0) ok = -1;
            close(tmpfd);

            if (ok == 0) {
                if (g_aof_fd >= 0) {
                    (void)fsync(g_aof_fd);
                    close(g_aof_fd);
                    g_aof_fd = -1;
                }
                if (rename(g_tmp_path, g_aof_path) == 0) { // 将临时文件重命名为新的AOF文件
                    int fd = open(g_aof_path, O_CREAT | O_APPEND | O_WRONLY, 0644); // 打开新的AOF文件并更新全局文件描述符
                    if (fd >= 0) {
                        g_aof_fd = fd;
                        g_last_fsync_ms = 0;
                    }
                }
            }
        }
    }

    (void)unlink(g_incr_path);
    if (!child_ok) {
        (void)unlink(g_tmp_path);
    }
    (void)old_pid;
    kvs_db_unlock();
    return NULL;
}


// 后台重写功能 调用rewrite_finalize_thread
int kvs_aof_bgrewriteaof_start(void) {
    const kvs_server_config_t *cfg = &kvs_server.config;
    if (!cfg->enable_persistence || !cfg->enable_aof) {
        return 0;
    }

    pthread_mutex_lock(&g_rewrite_mu);
    if (g_rewrite_child_pid > 0) {
        pthread_mutex_unlock(&g_rewrite_mu);
        return 1; /* 正在运行 */
    }
    pthread_mutex_unlock(&g_rewrite_mu);

    if (g_aof_path[0] == '\0') {
        const char *aof_path = (cfg->aof_path[0] != '\0') ? cfg->aof_path : "log/appendonly.aof";
        (void)snprintf(g_aof_path, sizeof(g_aof_path), "%s", aof_path);
        (void)snprintf(g_tmp_path, sizeof(g_tmp_path), "%s.tmp", aof_path);
        (void)snprintf(g_incr_path, sizeof(g_incr_path), "%s.incr", aof_path);
    }

    if (ensure_log_dir_exists() != 0) {
        return -1;
    }

    (void)unlink(g_tmp_path);
    (void)unlink(g_incr_path);

    int incrfd = open(g_incr_path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (incrfd < 0) {
        return -1;
    }

    /* 创建子进程以写入快照AOF */
    pid_t pid = fork();
    if (pid < 0) {
        close(incrfd);
        (void)unlink(g_incr_path);
        return -1;
    }

    if (pid == 0) {
        /* 子进程：请勿触碰 pthread 锁；仅写入快照 */
        if (g_aof_fd >= 0) close(g_aof_fd);
        close(incrfd);

        int tmpfd = open(g_tmp_path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
        if (tmpfd < 0) _exit(1);
        if (aof_write_snapshot_to_fd(tmpfd) != 0) {
            close(tmpfd);
            _exit(1);
        }
        if (fsync(tmpfd) != 0) {
            close(tmpfd);
            _exit(1);
        }
        close(tmpfd);
        _exit(0);
    }

    /* 父节点：开始收集增量写入 */
    pthread_mutex_lock(&g_rewrite_mu);
    g_incr_fd = incrfd;
    g_rewrite_child_pid = pid;
    pthread_mutex_unlock(&g_rewrite_mu);

    /* 父进程：创建线程等待子进程并完成终结 */
    if (!g_rewrite_thread_started) {
        g_rewrite_thread_started = 1;
        if (pthread_create(&g_rewrite_thread, NULL, rewrite_finalize_thread, NULL) != 0) {
            pthread_mutex_lock(&g_rewrite_mu);
            g_rewrite_thread_started = 0;
            g_rewrite_child_pid = -1;
            if (g_incr_fd >= 0) close(g_incr_fd);
            g_incr_fd = -1;
            pthread_mutex_unlock(&g_rewrite_mu);
            (void)unlink(g_incr_path);
            return -1;
        }
        pthread_detach(g_rewrite_thread);
    } else {
        /* 重用同一线程变量；为简化起见，每次启动时分离 */
        if (pthread_create(&g_rewrite_thread, NULL, rewrite_finalize_thread, NULL) != 0) {
            pthread_mutex_lock(&g_rewrite_mu);
            g_rewrite_child_pid = -1;
            if (g_incr_fd >= 0) close(g_incr_fd);
            g_incr_fd = -1;
            pthread_mutex_unlock(&g_rewrite_mu);
            (void)unlink(g_incr_path);
            return -1;
        }
        pthread_detach(g_rewrite_thread);
    }

    return 0;
}

// 关闭和清理AOF
void kvs_aof_close(void) {
    /* 最佳努力：停止重写状态 */
    pthread_mutex_lock(&g_rewrite_mu);
    int incr_fd = g_incr_fd;
    g_incr_fd = -1;
    pid_t pid = g_rewrite_child_pid;
    g_rewrite_child_pid = -1;
    pthread_mutex_unlock(&g_rewrite_mu);

    if (incr_fd >= 0) {
        (void)fsync(incr_fd);
        close(incr_fd);
        (void)unlink(g_incr_path);
    }
    if (pid > 0) {
        int status = 0;
        (void)waitpid(pid, &status, 0);
        (void)unlink(g_tmp_path);
    }

    if (g_aof_fd >= 0) {
        (void)fsync(g_aof_fd);
        close(g_aof_fd);
        g_aof_fd = -1;
    }
}
