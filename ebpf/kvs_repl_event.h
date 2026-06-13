#ifndef KVS_REPL_EVENT_H
#define KVS_REPL_EVENT_H

#define KVS_REPL_EVENT_MAGIC 0x4B565352u /* 'KVSR' */

/*
 * ============================ ringbuf 事件：kvs_repl_event ============================
 *
 * 这是 eBPF -> 用户态 agent 的“最小消息”。
 *
 * 为什么事件只包含 (aof_start_off, aof_len) 而不是直接携带命令内容？
 *   - eBPF 不适合携带大 payload：栈/验证器限制、复制成本高。
 *   - 我们已经把写命令以 RESP 形式写入了 AOF 文件。
 *   - 因此只要告诉 agent “本次新追加的 AOF 段落在哪里、多长”，
 *     agent 就能用 pread() 从文件读取真实字节并通过网络转发。
 *
 * ABI/兼容性：
 *   - 该结构同时被 BPF 程序与用户态程序 include。
 *   - BPF 侧不能 include 标准库头（<stdint.h> 等），所以用 __u32/__u64。
 *   - 用户态侧用 stdint 的固定宽度类型。
 *   - 字段顺序和大小必须一致，否则 ringbuf 收到的数据会错位。
 *
 * 字段语义：
 *   - magic: 固定魔数，用于快速过滤错误/旧版本事件。
 *   - pid:   触发事件的进程 pid（主库 kvstore-server）。
 *   - ts_ns: 事件时间戳（bpf_ktime_get_ns），便于调试延迟/乱序。
 *   - aof_start_off/aof_len: 追加 AOF 的 [start, start+len) 字节区间。
 */

struct kvs_repl_event {
#if defined(__BPF__)
    __u32 magic;
    __u32 pid;
    __u64 ts_ns;
    __s64 aof_start_off;
    __s32 aof_len;
    __s32 reserved;
#else
    /* userspace */
    #include <stdint.h>
    uint32_t magic; // 魔数，用于验证事件有效性
    uint32_t pid;
    uint64_t ts_ns;
    int64_t  aof_start_off; // AOF起始偏移量
    int32_t  aof_len; // 数据长度
    int32_t  reserved;
#endif
};

#endif
