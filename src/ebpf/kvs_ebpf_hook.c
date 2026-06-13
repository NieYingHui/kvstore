#include "kvs_ebpf_hook.h"

KVS_NOINLINE
void kvs_ebpf_repl_hook(int argc, char * const *argv, int is_resp) {
    /*
     * 这是“稳定 uprobe 挂钩点”的默认实现：
     */
    (void)argc;
    (void)argv;
    (void)is_resp;
}

KVS_NOINLINE
void kvs_ebpf_aof_append_hook(long long start_off, int len) {
    /*
     * 同上：这是 AOF 追加事件的稳定挂钩点。
     *
     * eBPF uprobe 命中本函数时，会从寄存器参数中取到：
     *   - start_off: 本次 AOF 追加开始的偏移
     *   - len:       本次追加写入的字节数
     * 然后通过 ringbuf 把该事件送到用户态 agent。
     */
    (void)start_off;
    (void)len;
}
