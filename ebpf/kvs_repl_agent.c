#define _GNU_SOURCE

#include <bpf/libbpf.h>
#include <bpf/bpf.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>

#include "kvs_repl_event.h"

/*
 * ============================ kvs-repl-agent（用户态复制代理） ============================
 *
 * 角色定位：
 *   这是“把 eBPF 事件变成真正网络复制流”的用户态程序。
 *
 * 它做三件事：
 *   1) 加载/挂载 eBPF uprobe 程序（kvs_repl.bpf.o）到主库二进制的 hook 符号。
 *   2) 从 ringbuf 接收事件（kvs_repl_event）：得到 AOF 新增片段的 (offset, len)。
 *   3) 用 pread() 从 AOF 文件读取 [offset, offset+len) 原始字节，并通过 TCP 原样发送。
 *
 * 为什么转发“原始 AOF 字节”（RESP）？
 *   - AOF 本身就是以 RESP 形式记录写命令。
 *   - 复制流如果保持 RESP 原样，副本只需像“回放 AOF”一样处理即可。
 *   - 这避免了在 agent 里重新解析/重编码命令，减少复杂度与不一致风险。
 *
 * 数据一致性与容错（本 MVP 的策略）：
 *   - agent 维护 next_off：表示“已经成功发送到副本的 AOF 字节末尾偏移”。
 *   - 如果 ringbuf 丢事件、或网络中断导致 offset 出现空洞：
 *       使用 stream_aof_span(next_off, ev_off) 从 AOF 补齐缺口。
 *   - 如果发现 AOF 被截断/重写（end < next_off）：
 *       直接触发重新 fullsync（从 0 重放到当前 end）。
 *
 * 注意：
 *   - 这是一个 MVP：仅做“字节流复制”，不做认证/加密/压缩/心跳。
 *   - 生产化通常需要：握手协议、断点续传确认、主从角色切换等。
 */

static volatile sig_atomic_t g_stop = 0; // sig_atomic_t: 一种整数类型，保证在信号处理期间不会被中断

static void on_sigint(int signo) { // 处理SIGINT信号（通常是Ctrl+C触发的中断信号）
    (void)signo;
    g_stop = 1;
}

static int bump_memlock_rlimit(void) { // 内存锁定限制调整
    struct rlimit rlim = {0}; //  用于限制系统资源
    rlim.rlim_cur = RLIM_INFINITY; // 设置当前（软）限制为无限
    rlim.rlim_max = RLIM_INFINITY; // 设置最大（硬）限制为无限
    return setrlimit(RLIMIT_MEMLOCK, &rlim); //设置资源限制为内存锁定限制
}

struct agent_ctx {
    int aof_fd;
    int sock_fd;
    int enable_net;
    int verbose;
    char dst_host[256];
    char dst_port[16];

    long long next_off;
    int fullsync;
};

/*
 * next_off 的语义：
 *   - 它表示 agent 认为“副本已经收到并处理”的 AOF 末尾偏移。
 *   - 在没有副本确认机制的 MVP 里，我们只能假设 send() 成功即代表副本收到。
 *     这在真实网络中并不等价于“对端已落盘/已应用”，但足够用于实验/课程项目。
 */

static void close_sock(struct agent_ctx *actx) { // 关闭一个套接字(socket)
    if (!actx) return;
    if (actx->sock_fd >= 0) {
        close(actx->sock_fd);
        actx->sock_fd = -1;
    }
}

// 解析包含主机名和端口号的字符串
static int parse_hostport(const char *s, char *host, size_t host_cap, char *port, size_t port_cap) {
    if (!s || !host || !port) return -1;
    const char *colon = strrchr(s, ':'); // 查找分隔符
    if (!colon || colon == s || *(colon + 1) == '\0') return -1;
    size_t hlen = (size_t)(colon - s);
    size_t plen = strlen(colon + 1);
    if (hlen + 1 > host_cap) return -1;
    if (plen + 1 > port_cap) return -1;
    memcpy(host, s, hlen);
    host[hlen] = '\0';
    memcpy(port, colon + 1, plen + 1);
    return 0;
}

static int connect_dst(struct agent_ctx *actx) { // 连接到目标服务器
    if (!actx) return -1;
    if (!actx->enable_net) return 0;
    if (actx->sock_fd >= 0) return 0;

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    // getaddrinfo将目标主机名和端口转换为套接字地址结构。
    int rc = getaddrinfo(actx->dst_host, actx->dst_port, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "getaddrinfo(%s:%s) failed: %s\n", actx->dst_host, actx->dst_port, gai_strerror(rc));
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        fprintf(stderr, "connect(%s:%s) failed: %s\n", actx->dst_host, actx->dst_port, strerror(errno));
        return -1;
    }
    actx->sock_fd = fd;
    if (actx->verbose) fprintf(stdout, "connected to %s:%s\n", actx->dst_host, actx->dst_port);
    return 0;
}

/*
 * 副本响应“后台丢弃”（默认启用，最小改动）：
 *
 * 现状：副本端会像普通服务一样对每个写命令回 RESP 响应；但复制 agent
 * 只负责 send() 写命令字节流，并不会读取响应。
 *
 * 风险：如果长期不读，副本->agent 方向的 TCP 接收缓冲区会逐渐被响应填满，
 * 进而导致：
 *   - 副本侧 send() 受阻（回包堵塞）
 *   - 最终影响副本处理/连接状态，甚至反过来拖慢复制
 *
 * 解决：在每次发送后，用非阻塞 recv(MSG_DONTWAIT) 把已到达的响应字节读空并丢弃。
 * 这不会改变复制出去的字节内容（仍是 AOF 原始 RESP），只是避免 TCP 反压。
 */
static void drain_replica_replies(struct agent_ctx *actx) {
    if (!actx || !actx->enable_net) return;
    if (actx->sock_fd < 0) return;

    char buf[4096];
    for (;;) {
        ssize_t n = recv(actx->sock_fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n > 0) {
            /* 丢弃即可；如需调试可在 verbose 下统计字节数，但默认不输出避免刷屏 */
            continue;
        }
        if (n == 0) {
            /* 对端关闭 */
            close_sock(actx);
            return;
        }
        /* n < 0 */
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* 当前没有更多可读数据 */
            return;
        }
        /* 其它错误：视为连接失效，交给上层重连/追赶 */
        close_sock(actx);
        return;
    }
}

static int send_all(struct agent_ctx *actx, const void *data, size_t len) {
    /*
     * send_all：把一个内存缓冲完整写到 TCP 连接。
     *
     * 这里不做 framing（分帧/长度前缀），因为我们发送的是 RESP 字节流：
     *   - RESP 自带边界（\r\n 和 bulk length），副本按流解析即可。
     *
     * 任何 send 错误都视为连接失效：
     *   - 关闭 socket
     *   - 上层会尝试重连并 catch-up
     */
    if (!actx || !data) return -1;
    if (!actx->enable_net) return 0;
    if (connect_dst(actx) != 0) return -1;

    const char *p = (const char *)data;
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(actx->sock_fd, p + off, len - off, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            close_sock(actx);
            return -1;
        }
        if (n == 0) {
            close_sock(actx);
            return -1;
        }
        off += (size_t)n;
    }

    /* 默认丢弃副本响应，避免 TCP 反压导致复制卡住。 */
    drain_replica_replies(actx);
    return 0;
}

// 从AOF文件中读取指定范围的数据并发送
static int stream_aof_range(struct agent_ctx *actx, long long start_off, int len) {
    /*
     * 从 AOF 文件读取一个精确区间并发送：
     *   - 使用 pread：不会改变文件描述符的共享偏移，线程安全更好。
     *   - 分块读取：避免一次读太大。
     */
    if (!actx || actx->aof_fd < 0 || start_off < 0 || len <= 0) return -1;

    char buf[4096];
    long long off = start_off;
    int remaining = len;

    while (remaining > 0) {
        int chunk = remaining;
        if (chunk > (int)sizeof(buf)) chunk = (int)sizeof(buf);

// 使用pread系统调用(而不是read)，因为它不会改变文件描述符的偏移量，更适合多线程环境
        ssize_t n = pread(actx->aof_fd, buf, (size_t)chunk, (off_t)off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;

        if (send_all(actx, buf, (size_t)n) != 0) {
            return -1;
        }
        off += (long long)n;
        remaining -= (int)n;
    }

    return 0;
}

static long long aof_end_off(struct agent_ctx *actx) { // 获取AOF文件的当前结束位置
    if (!actx || actx->aof_fd < 0) return -1;
    // 使用lseek系统调用获取文件描述符actx->aof_fd指向文件的当前结束位置
    off_t end = lseek(actx->aof_fd, (off_t)0, SEEK_END);
    if (end == (off_t)-1) return -1;
    return (long long)end;
}

static int stream_aof_span(struct agent_ctx *actx, long long start_off, long long end_off) {
    if (!actx) return -1;
    if (start_off < 0 || end_off < start_off) return -1;
    long long remaining = end_off - start_off;
    long long off = start_off;
    while (remaining > 0) {
        // 每次处理的最大块大小限制为1MB(1 << 20)
        int chunk = (remaining > (long long)(1 << 20)) ? (1 << 20) : (int)remaining;
        if (stream_aof_range(actx, off, chunk) != 0) return -1;
        off += (long long)chunk;
        remaining -= (long long)chunk;
    }
    return 0;
}

static int do_fullsync(struct agent_ctx *actx) { // 把当前 AOF [0, end) 全量重放到副本。
    /*
     * fullsync：把当前 AOF [0, end) 全量重放到副本。
     *
     * 适用场景：
     *   - 副本刚加入，完全没有历史。
     *   - agent 发现 AOF 被截断/重写，无法用增量补齐。
     */
    if (!actx || !actx->enable_net) return 0;
    if (connect_dst(actx) != 0) return -1;

    long long end0 = aof_end_off(actx);
    if (end0 < 0) {
        fprintf(stderr, "fullsync: failed to get AOF end offset: %s\n", strerror(errno));
        return -1;
    }

    if (actx->verbose) {
        fprintf(stdout, "fullsync: replay AOF [0, %lld)\n", end0);
    }

    if (stream_aof_span(actx, 0, end0) != 0) {
        fprintf(stderr, "fullsync failed\n");
        return -1;
    }

    actx->next_off = end0;
    return 0;
}

// 当网络抖动/重连后，主动把 AOF 从 next_off 补齐到文件末尾。
static int catch_up_to_end(struct agent_ctx *actx) {
    /*
     * catch_up：当网络抖动/重连后，主动把 AOF 从 next_off 补齐到文件末尾。
     *
     * 这是“事件驱动 + 周期性自愈”的简化版：
     *   - 平时靠 ringbuf 事件触发增量复制。
     *   - 出错时就扫一遍 AOF 末尾，确保不漏。
     */
    if (!actx || !actx->enable_net) return 0;
    if (connect_dst(actx) != 0) return -1;

    long long end = aof_end_off(actx);
    if (end < 0) return -1;

    if (actx->next_off > end) {
        /* AOF 截断/轮换。最安全操作：重新执行全同步 */
        if (actx->verbose) {
            fprintf(stdout, "AOF end (%lld) < next_off (%lld), re-fullsync\n", end, actx->next_off);
        }
        actx->next_off = 0;
        return do_fullsync(actx);
    }

    if (end > actx->next_off) {
        if (actx->verbose) {
            fprintf(stdout, "catch-up: replay AOF [%lld, %lld)\n", actx->next_off, end);
        }
        if (stream_aof_span(actx, actx->next_off, end) != 0) return -1;
        actx->next_off = end;
    }
    return 0;
}

// 处理主库到副本的数据同步
static int handle_event(void *ctx, void *data, size_t data_sz) {
    /*
     * ringbuf 回调：每条事件表示“主库刚刚向 AOF 追加了一段字节”。
     *
     * 我们需要把这段字节可靠地发给副本：
     *   - 如果事件完全落在 next_off 之前：说明已发送，忽略。
     *   - 如果事件 offset 大于 next_off：说明中间缺了一段（可能丢事件/断线），先补洞。
     *   - 再发送事件对应的 [off, off+len) 。
     */
    struct agent_ctx *actx = (struct agent_ctx *)ctx;
    const struct kvs_repl_event *e = (const struct kvs_repl_event *)data;

    if (data_sz < sizeof(*e)) return 0;
    if (e->magic != KVS_REPL_EVENT_MAGIC) return 0;

    if (actx->verbose) { // 启用详细模式(verbose)
        fprintf(stdout, "event pid=%u off=%lld len=%d\n", e->pid, (long long)e->aof_start_off, (int)e->aof_len);
    }

    if (actx->enable_net) {
        long long ev_off = (long long)e->aof_start_off; // 起始偏移量
        int ev_len = (int)e->aof_len; // 长度
        long long ev_end = ev_off + (long long)ev_len; // 结束位置

        if (ev_off < 0 || ev_len <= 0) return 0;

        /* 跳过我们已发送内容中完全涵盖的部分。 */ // 事件是否完全在已发送的数据范围内
        if (actx->next_off > 0 && ev_end <= actx->next_off) {
            return 0;
        }

        /* 若存在缺口（套接字断开或环形缓冲区丢失），则从AOF补全数据。 */
        if (actx->next_off >= 0 && ev_off > actx->next_off) {
            if (stream_aof_span(actx, actx->next_off, ev_off) != 0) {
                close_sock(actx);
                (void)connect_dst(actx);
                (void)catch_up_to_end(actx);
            }
        }
        // 传输当前事件对应的数据范围
        if (stream_aof_range(actx, ev_off, ev_len) != 0) {
            close_sock(actx);
            (void)connect_dst(actx);
            (void)catch_up_to_end(actx);
            if (stream_aof_range(actx, ev_off, ev_len) != 0) {
                fprintf(stderr, "stream AOF range failed: off=%lld len=%d errno=%d\n",
                        ev_off, ev_len, errno);
                return 0;
            }
        }

        if (ev_end > actx->next_off) actx->next_off = ev_end; //  更新同步位置
    }

    return 0;
}

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s --bin <kvstore-server> --aof <appendonly.aof> --offset <hex> [--pid <pid>] [--dst host:port] [--verbose] [--no-fullsync]\n"
            "\n"
            "This MVP agent loads an eBPF uprobe program that listens for AOF append events\n"
            "(kvs_ebpf_aof_append_hook), then reads the appended bytes from the AOF file\n"
            "and forwards them to a replica over TCP (raw RESP bytes).\n",
            prog);
}

int main(int argc, char **argv) {
    const char *bin_path = NULL;
    const char *aof_path = NULL;
    long long uprobe_offset = -1;
    int pid = -1;
    const char *dst = NULL;
    int verbose = 0;
    int fullsync = 1;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--bin") == 0 && i + 1 < argc) {
            bin_path = argv[++i];
        } else if (strcmp(argv[i], "--aof") == 0 && i + 1 < argc) {
            aof_path = argv[++i];
        } else if (strcmp(argv[i], "--offset") == 0 && i + 1 < argc) {
            const char *s = argv[++i];
            errno = 0;
            unsigned long long v = strtoull(s, NULL, 0);
            if (errno != 0) {
                fprintf(stderr, "invalid --offset: %s\n", s);
                return 2;
            }
            uprobe_offset = (long long)v;
        } else if (strcmp(argv[i], "--pid") == 0 && i + 1 < argc) {
            pid = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--dst") == 0 && i + 1 < argc) {
            dst = argv[++i];
        } else if (strcmp(argv[i], "--verbose") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "--no-fullsync") == 0) {
            fullsync = 0;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (!bin_path || !aof_path || uprobe_offset < 0) {
        usage(argv[0]);
        fprintf(stderr,
                "\nMissing required args. To get --offset, run something like:\n"
                "  nm -n <kvstore-server> | grep kvs_ebpf_aof_append_hook\n");
        return 2;
    }

    if (bump_memlock_rlimit() != 0) {
        fprintf(stderr, "setrlimit(RLIMIT_MEMLOCK) failed: %s\n", strerror(errno));
        // 在所有发行版/容器中并非致命错误
    }

    struct agent_ctx actx;
    memset(&actx, 0, sizeof(actx));
    actx.aof_fd = open(aof_path, O_RDONLY | O_CLOEXEC);
    if (actx.aof_fd < 0) {
        fprintf(stderr, "open aof failed: %s\n", strerror(errno));
        return 1;
    }

    actx.sock_fd = -1;
    actx.enable_net = 0;
    actx.verbose = verbose;
    actx.next_off = 0;
    actx.fullsync = fullsync;
    actx.dst_host[0] = '\0';
    actx.dst_port[0] = '\0';
    if (dst) {
        if (parse_hostport(dst, actx.dst_host, sizeof(actx.dst_host), actx.dst_port, sizeof(actx.dst_port)) != 0) {
            fprintf(stderr, "invalid --dst, expected host:port\n");
            close(actx.aof_fd);
            return 2;
        }
        actx.enable_net = 1;
        (void)connect_dst(&actx);

        if (actx.fullsync) {
            if (do_fullsync(&actx) != 0) {
                fprintf(stderr, "initial fullsync failed; will still try incremental\n");
                /* 最佳努力：从当前末端开始 */
                long long end0 = aof_end_off(&actx);
                if (end0 > 0) actx.next_off = end0;
            }
        } else {
            long long end0 = aof_end_off(&actx);
            if (end0 > 0) actx.next_off = end0;
        }
    }

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    libbpf_set_strict_mode(LIBBPF_STRICT_ALL); // 设置libbpf严格模式

    /*
     * 加载 BPF：
     *   - kvs_repl.bpf.o 是内核侧 uprobe 程序（clang -target bpf 编译产物）。
     *   - bpf_object__load 会完成验证/加载。
     */

    struct bpf_object *obj = NULL;
    struct bpf_program *prog = NULL;
    struct bpf_link *link = NULL;

    obj = bpf_object__open_file("kvs_repl.bpf.o", NULL);
    if (!obj) {
        fprintf(stderr, "bpf_object__open_file failed\n");
        return 1;
    }

    if (bpf_object__load(obj) != 0) {
        fprintf(stderr, "bpf_object__load failed: %s\n", strerror(errno));
        bpf_object__close(obj);
        return 1;
    }

    // 查找BPF程序
    prog = bpf_object__find_program_by_name(obj, "uprobe_kvs_ebpf_aof_append_hook");
    if (!prog) {
        fprintf(stderr, "find program failed\n");
        bpf_object__close(obj);
        return 1;
    }
//  附加uprobe
    link = bpf_program__attach_uprobe(prog, false /* retprobe */, pid, bin_path, (size_t)uprobe_offset);

    /*
     * attach_uprobe 的关键参数：
           prog: 要附加的BPF程序
           false: 不是retprobe（不返回时触发）
     *   - pid:  -1 表示所有进程；指定 pid 表示只跟踪某个 kvstore-server。
     *   - bin_path + uprobe_offset: uprobe 需要知道目标函数在 ELF 里的偏移。
     *
     * 如何获得 offset：
     *   nm -n build/bin/kvstore-server | grep kvs_ebpf_aof_append_hook
     *
     * 注意：不同平台/是否 strip/不同编译选项可能影响符号；调试时以你的二进制为准。
     */
    if (!link) {
        fprintf(stderr, "attach uprobe failed: %s\n", strerror(errno));
        bpf_object__close(obj);
        return 1;
    }

    // 获取ringbuf映射文件描述符
    int map_fd = bpf_object__find_map_fd_by_name(obj, "events");
    if (map_fd < 0) {
        fprintf(stderr, "find ringbuf map failed\n");
        bpf_link__destroy(link);
        bpf_object__close(obj);
        return 1;
    }

    struct ring_buffer *rb = ring_buffer__new(map_fd, handle_event, &actx, NULL);

    /*
     * 进入事件循环：
     *   ring_buffer__poll 会阻塞最多 250ms。
     *   收到事件就回调 handle_event。
     */
    if (!rb) {
        fprintf(stderr, "ring_buffer__new failed\n");
        bpf_link__destroy(link);
        bpf_object__close(obj);
        return 1;
    }

    fprintf(stdout, "listening... Ctrl-C to stop\n");
    while (!g_stop) { // 启动事件循环
        int err = ring_buffer__poll(rb, 250 /* ms */); // 当收到事件时，会调用handle_event回调函数
        if (err == -EINTR) break;
        if (err < 0) {
            fprintf(stderr, "ring_buffer__poll: %d\n", err);
            break;
        }
    }

    ring_buffer__free(rb);
    bpf_link__destroy(link);
    bpf_object__close(obj);
    close_sock(&actx);
    close(actx.aof_fd);
    return 0;
}
