// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
// Minimal uprobe -> 用于 kvstore AOF 流的环形缓冲区事件生成器。

/*
 * =============================== 本文件在做什么？ ===============================
 *
 * 这是运行在内核态的 eBPF 程序（uprobe）。
 *
 * 它 attach 到 kvstore-server 进程里的用户态函数：
 *   kvs_ebpf_aof_append_hook(start_off, len)
 *
 * 每当主库成功追加一条 AOF 记录后，该 hook 会被调用（见 src/persistence/kvs_aof.c）。
 * uprobe 命中后，本 BPF 程序会：
 *   1) 从 CPU 寄存器参数中取出 start_off、len
 *   2) 通过 ring buffer（BPF_MAP_TYPE_RINGBUF）发一个小事件给用户态
 *
 * 重要：
 *   - BPF 程序“不读取 AOF 文件内容，不发网络”。
 *   - 它只发“位置+长度”这类非常小且固定的元数据。
 *   - 真正的字节读取与网络转发在用户态 agent 中完成（ebpf/kvs_repl_agent.c）。
 *
 * 为什么要用 uprobe？
 *   - 复制的触发点在用户态业务逻辑里：AOF 追加完成的那一刻。
 *   - uprobe 可以在不改内核、不写内核模块的情况下，从内核观察用户态函数调用。
 *
 * 你需要知道的现实约束：
 *   - uprobe attach 需要 “二进制路径 + 符号偏移”。本项目的 agent 通过 nm/objdump
 *     获取偏移（运行参数 --offset）。
 *   - 不同编译选项/strip 可能影响符号和偏移；因此 hook 函数被标记为 noinline，
 *     减少被优化掉的概率。
 */

/*
 * 某些精简环境未提供完整的内核 uapi 头文件用于 BPF 构建。
 * libbpf 的 bpf_helper_defs.h 使用了 __u32/__u64 等类型，因此需本地定义这些类型
 * 以避免包含 <linux/types.h>（该文件可能依赖缺失的 asm/types.h）。
 */
typedef unsigned char __u8;
typedef unsigned short __u16;
typedef unsigned int __u32;
typedef unsigned long long __u64;
typedef signed char __s8;
typedef signed short __s16;
typedef signed int __s32;
typedef signed long long __s64;

/* bpf_helper_defs.h also references these network/checksum types. */
typedef __u16 __be16;
typedef __u32 __be32;
typedef __u32 __wsum;

/*
 * 某些 libbpf 头文件仅对 struct pt_regs 进行了前向声明。提供一个最简定义，
 * 使 PT_REGS_PARMn 宏能在不依赖内核头文件的情况下编译通过。
 */
#if defined(__TARGET_ARCH_x86)
struct pt_regs {
    __u64 r15;
    __u64 r14;
    __u64 r13;
    __u64 r12;
    __u64 rbp;
    __u64 rbx;
    __u64 r11;
    __u64 r10;
    __u64 r9;
    __u64 r8;
    __u64 rax;
    __u64 rcx;
    __u64 rdx;
    __u64 rsi;
    __u64 rdi;
    __u64 orig_rax;
    __u64 rip;
    __u64 cs;
    __u64 eflags;
    __u64 rsp;
    __u64 ss;
};
#elif defined(__TARGET_ARCH_arm64)
struct pt_regs {
    __u64 regs[31];
    __u64 sp;
    __u64 pc;
    __u64 pstate;
};
#endif

/* 最小化头文件中可能缺少地图类型枚举。 */
#ifndef BPF_MAP_TYPE_RINGBUF
#define BPF_MAP_TYPE_RINGBUF 27
#endif

#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

#include "kvs_repl_event.h"

char LICENSE[] SEC("license") = "Dual BSD/GPL";

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 1 << 24); /* 环形队列大小 16MB */
} events SEC(".maps");

/*
 * ringbuf 的直觉理解：
 *   - 内核侧：reserve 一块空间，写入事件数据，然后 submit。
 *   - 用户态：用 libbpf ring_buffer__new/poll 读取事件。
 *
 * 为什么用 ringbuf 而不是 perf buffer：
 *   - ringbuf API 更直接（无需 per-cpu buffer），开销更低。
 *   - 适合这种“高频、小消息”的事件上报。
 */

static __always_inline __u64 now_ns(void) {
    return bpf_ktime_get_ns();
}

SEC("uprobe/kvs_ebpf_aof_append_hook") // 是一个uprobe（用户级探针），用于监控用户空间函数kvs_ebpf_aof_append_hook的调用
int uprobe_kvs_ebpf_aof_append_hook(struct pt_regs *ctx) { // 这个程序用于监控AOF追加操作
    struct kvs_repl_event *e;

    long long start_off = (long long)PT_REGS_PARM1(ctx); // 提取参数
    int len = (int)PT_REGS_PARM2(ctx);

    /*
     * 参数来源说明：
     *   - uprobe 的 ctx 是 struct pt_regs*，保存了进入被跟踪函数时的寄存器上下文。
     *   - PT_REGS_PARM1/2 宏会根据不同架构（x86_64/arm64）取出第 1/2 个参数。
     *   - 这里我们约定 hook 的签名固定为 (long long start_off, int len)，
     *     agent 和 BPF 两端必须一致。
     */

    if (len <= 0) return 0;

    e = bpf_ringbuf_reserve(&events, sizeof(*e), 0); // 在环形缓冲区中预留空间用于存储事件
    if (!e) return 0;

    /*
     * 事件内容尽量保持“固定大小、低成本”。
     * eBPF 侧只写元信息：
     *   - aof_start_off/aof_len: 指向 AOF 文件中新追加的数据段
     * 用户态会用这两个值去 pread AOF 文件并把原始字节转发出去。
     */

    e->magic = KVS_REPL_EVENT_MAGIC;
    e->pid = (__u32)(bpf_get_current_pid_tgid() >> 32);
    e->ts_ns = now_ns();
    e->aof_start_off = (__s64)start_off;
    e->aof_len = (__s32)len;
    e->reserved = 0;

    bpf_ringbuf_submit(e, 0); // 提交事件到环形缓冲区，供用户态程序读取
    return 0;
}
