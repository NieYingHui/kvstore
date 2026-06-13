# kvstore eBPF replication (MVP)

This directory contains an **optional** eBPF-based replication skeleton:

- A uprobe program emits events when kvstore appends a command to AOF.
- A userspace agent listens on a ringbuf and reads the appended bytes from the AOF file.

It is **not** built by default. Build it explicitly with `make ebpf` from the project root.

## 架构与原理（建议先读）

这套复制是典型的 “eBPF 只发事件 + 用户态做重活” 模式。

核心事实：
- kvstore 主库在每次写入成功后，会把写命令以 RESP 格式追加到 AOF 文件（appendonly.aof）。
- 追加成功后，会调用一个空函数 hook：`kvs_ebpf_aof_append_hook(start_off, len)`（符号稳定、可被 uprobe 挂载）。
- eBPF uprobe 程序（kvs_repl.bpf.c）监听这个 hook：
  - 从寄存器参数拿到 (start_off, len)
  - 通过 ringbuf 发送一个小事件给用户态
- 用户态 agent（kvs_repl_agent.c）收到事件后：
  - 用 pread() 从 AOF 文件读取 [start_off, start_off+len) 的原始字节
  - 通过 TCP 原样转发给副本

简化的端到端流程：

1) 主库执行写命令成功
2) 主库追加 AOF（写入 RESP 字节）
3) 主库触发 hook（仅用于 uprobe 命中）
4) eBPF 发事件（offset+len）
5) agent 读 AOF 字节并发给副本
6) 副本像“回放 AOF”一样解析并应用

为什么不让 eBPF 直接复制 payload：
- eBPF 不适合做文件 I/O、网络发送
- 复制 payload 可能很大，放进 ringbuf 会造成不必要的拷贝/开销
- AOF 已经是“事实日志”，最适合做增量复制源

## Prerequisites (Linux)

Ubuntu example:

```bash
sudo apt-get update
sudo apt-get install -y clang llvm gcc make pkg-config libbpf-dev libelf-dev zlib1g-dev
```

## Build

From the project root:

```bash
make ebpf
```

Artifacts:
- `ebpf/kvs_repl.bpf.o`
- `ebpf/kvs-repl-agent`

## Run (MVP: prints events + reads AOF bytes)

1) 确保在服务器配置中启用了AOF（复制由AOF追加驱动）。
2) 按常规方式构建服务器二进制文件。
3) 获取uprobe钩子的符号偏移量：

```bash
nm -n build/bin/kvstore-server | grep kvs_ebpf_aof_append_hook
```

如果 nm 输出看不懂，可用：

```bash
objdump -t build/bin/kvstore-server | grep kvs_ebpf_aof_append_hook
```

调试提示：
- 如果 attach 失败，先确认二进制未被 strip 掉符号，或至少该符号仍可解析。
- 符号偏移与二进制强相关：重新编译后 offset 可能变化。

4) 运行代理程序（需要加载BPF的权限）：

```bash
cd ebpf
sudo ./kvs-repl-agent \
  --bin ../build/bin/kvstore-server \
  --aof ../log/appendonly.aof \
  --offset 0x82c0  \
  --dst 127.0.0.1:3000 \
  --verbose
```

注释：
- 代理将 AOF 中的 **原始响应字节** 转发至副本 TCP 端口。
- 在目标端口启动第二个 kvstore-server 作为副本。
- 默认情况下，代理连接副本后会执行**全量同步**，即从头开始重放AOF日志，以便晚加入的副本获取历史数据。
- 若仅需转发新写入数据，请使用`--no-fullsync`参数。

## 代码入口速查

- hook 声明与说明：`src/ebpf/kvs_ebpf_hook.h`
- hook 调用点（AOF 追加后触发）：`src/persistence/kvs_aof.c`
- BPF 程序（uprobe -> ringbuf）：`ebpf/kvs_repl.bpf.c`
- 事件结构（BPF/用户态共享 ABI）：`ebpf/kvs_repl_event.h`
- 用户态 agent（ringbuf -> pread -> TCP）：`ebpf/kvs_repl_agent.c`

uprobe机制：

  用于监控用户空间函数的调用
  需要指定目标二进制文件和函数偏移量
  可以监控函数入口和返回
ringbuffer通信：

  提供高效的内核-用户空间数据传输
  支持异步事件通知
  避免了传统方法的开销
