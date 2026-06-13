#ifndef KVS_EBPF_HOOK_H
#define KVS_EBPF_HOOK_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_MSC_VER)
#define KVS_NOINLINE __declspec(noinline)
#else
#define KVS_NOINLINE __attribute__((noinline))
#endif

/*
 * ============================ 这套 eBPF 主从同步的核心思路 ============================
 *
 * 目标：
 *   在不把大量业务数据塞进 eBPF 程序的前提下，实现“主库写入 -> 副本按相同顺序回放”。
 *
 * 关键约束（为什么要这么设计）：
 *   1) eBPF 程序运行在内核态，受限于：
 *      - 指令数/栈大小等限制
 *      - 不允许做复杂 I/O（不能直接 read 文件/发网络）
 *      - 从用户态内存读取大块数据会有开销和安全限制
 *   2) 复制需要传输的是“写入负载”（命令/变更），它可能很大（大 value、批量写等）。
 *
 * 因此本项目采用“eBPF 只负责发事件（小而固定）+ 用户态 agent 负责读真实数据并转发”的模式：
 *
 *   [kvstore-server 主进程]
 *       |  (1) 执行写命令成功
 *       |  (2) 追加 AOF：把写命令以 RESP 形式写入 appendonly.aof
 *       |  (3) 调用一个“稳定符号”的空函数（本文件声明的 hook）
 *       v
 *   [内核 eBPF uprobe]
 *       |  监听到 hook 被调用（uprobe 命中）
 *       |  从寄存器参数中取到 (start_off, len)
 *       |  通过 ringbuf 往用户态发送一个小事件：{offset, len, pid, ts}
 *       v
 *   [用户态 kvs-repl-agent]
 *       |  从 ringbuf 收到事件
 *       |  用 pread() 从 AOF 文件读取 [offset, offset+len) 的真实字节
 *       |  通过 TCP 原样转发给副本（副本端按 RESP 回放即可）
 *       v
 *   [kvstore-server 副本]
 *
 * 这两个 hook 的意义：
 *   - 它们被标记为 noinline，目的是让编译器不要内联/优化掉符号，确保 uprobe
 *     可以“稳定地”挂到这个函数入口。
 *   - 默认实现故意为空：不会改变业务逻辑；是否启用复制完全由外部 agent 决定。
 */

/*
 * 基于eBPF的复制机制的稳定uprobe挂钩点。
 *
 * 当写入命令在主节点成功应用后，将调用此函数。
 * 外部用户空间代理可将uprobe挂载至此符号，
 * 从用户内存读取argv参数，将写入操作复制至副本节点。
 *
 * 注意：默认实现故意留空。
 *
 * 现状说明（结合本仓库的实现）：
 *   当前 eBPF MVP 版本主要采用“基于 AOF 追加事件”的复制（见 kvs_ebpf_aof_append_hook）。
 *   该 repl_hook 仍保留为扩展点：
 *     - 未来可实现“直接从 argv 复制”（小 payload 时更低延迟）
 *     - 或者做一些轻量埋点/观测
 */
KVS_NOINLINE
void kvs_ebpf_repl_hook(int argc, char * const *argv, int is_resp);

/*
 * 基于AOF的复制流传输的稳定uprobe钩点。
 *
 * 在AOF条目成功追加后调用。
 * 代理可使用(start_off, len)从AOF文件读取精确字节数
 * 并转发至副本，避免在eBPF中进行大容量负载复制。
 *
 * 为什么参数是 (start_off, len)：
 *   - AOF 文件是顺序追加的日志。
 *   - 每次追加写命令后，我们能在用户态准确知道“本次追加从哪里开始、写了多少字节”。
 *   - eBPF 只需要把这两个整数通知给用户态 agent，agent 再去文件里读真实字节。
 *
 * 重要语义：
 *   - 该 hook 只在“确定 AOF 已成功写入”之后调用（否则副本会读到不存在的数据）。
 *   - start_off/len 指向的是“刚追加的那一段”，这使得增量复制非常自然。
 */
KVS_NOINLINE
void kvs_ebpf_aof_append_hook(long long start_off, int len);

#ifdef __cplusplus
}
#endif

#endif
