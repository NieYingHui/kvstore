# Bench 使用步骤（Linux）

目标：对比 **三种内存策略**（glibc / jemalloc / 内存池）在 **四种结构**（array / rbtree / hash / skiplist）上的吞吐与内存占用。

## 0. 准备

- 进入目录：`cd kvstore`
- 确保可执行权限：`chmod +x bench/run_matrix.sh`
- 建议关闭持久化减少 IO 干扰：脚本默认用 bench 配置文件 [bench/bench.conf](bench/bench.conf)

## 1. 一键跑矩阵并生成 CSV

最常用（输出到 result.csv）：

`COUNT=50000 VALUE_SIZE=32 ./bench/run_matrix.sh > ./bench/out/result.csv`

参数含义：
- `COUNT`：每轮请求数（每个组合会跑一次 SET + 一次 GET）
- `VALUE_SIZE`：value 固定字节数（0 表示按 v{i} 这种短字符串）

jemalloc 方式：脚本默认用 `LD_PRELOAD`，如果你的 jemalloc so 不在默认路径，指定：

`JEMALLOC_SO=/path/to/libjemalloc.so.2 COUNT=200000 VALUE_SIZE=32 ./bench/run_matrix.sh > result.csv`

## 2. 自动出图

安装 matplotlib（如果服务器没装）：

`python3 -m pip install --user matplotlib`

出图命令：

`python3 bench/plot_results.py --csv ./bench/out/result.csv --out-dir bench/plots`

输出文件：
- `bench/plots/throughput_<struct>.png`：SET/GET 吞吐（cmds/s）
- `bench/plots/memory_<struct>.png`：内存占用（VmRSS/VmHWM，MiB）

## 3. 如何解读

- 主要看 `throughput_*.png`：同一结构下三种 mode 的柱状对比。
- 内存看 `memory_*.png`：写入后的常驻内存（RSS）与历史峰值（HWM）。

建议：每次测试前保证机器空闲、固定 CPU 频率/隔离干扰、至少跑 2~3 次取中位数。


---

## mmap vs fwrite/fread（持久化 I/O 对比）

持久化实现中：
- RDB 写入走 `fwrite`；RDB 加载走 `mmap`
- AOF 追加走 `write`；AOF 回放走 `mmap`

为了“证明 mmap 在加载/恢复（读路径）更快”，这里提供一个**不改现有代码**的 I/O micro-benchmark（推荐只看随机读）：

### 为什么随机读更能体现 mmap 优势

项目里 mmap 的使用点在 **AOF 回放 / RDB 加载**，本质属于“读 + 解析”的路径；而随机读对比更贴近实际原因是：
- `pread/fread`：每次读都要走 syscall，并把数据从内核页缓存拷贝到用户缓冲区（一次额外拷贝）。
- `mmap`：文件页直接映射到进程地址空间；随机访问时主要成本是 page fault + 页缓存命中后的内存访问，省掉大量 syscall/拷贝。

因此，“随机读 mmap vs pread”的提升更稳定、更容易作为报告中的主证据。

### 结果怎么写进报告（建议口径：中位数 + 倍数）

以 `RAND_OPS=20000`、`RAND_BLOCK=4KiB` 的随机读为例：
- AOF：mmap 中位数约 **735.8 MiB/s**，pread 中位数约 **413.5 MiB/s**，提升约 **1.78×**。
- RDB：mmap 中位数约 **747.4 MiB/s**，pread 中位数约 **411.3 MiB/s**，提升约 **1.82×**。

> 建议：图上用“中位数 + p10~p90”误差条，能避免偶发抖动影响结论。

### 1) 生成 CSV（读：mmap vs fread/pread；写：mmap vs fwrite）

真实持久化文件为输入（推荐：先跑一次 server 产生 `log/dump.rdb` 或 `log/appendonly.aof`）：

`python3 bench/persist_io_bench.py --read-file log/appendonly.aof --workloads read_rand --iters 5 --rand-ops 20000 --rand-block 4KiB --out bench/out/io_compare.csv`

如果你想尽量消除 page cache 影响（需要 root）：

`python3 bench/persist_io_bench.py --read-file log/appendonly.aof --workloads read_rand --iters 5 --drop-caches-cmd "sudo sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches'" --out bench/out/io_compare.csv`

### 2) 出图（柱状图：中位数 + p10~p90）

`python3 bench/plot_io_compare.py bench/out/io_compare.csv --ops read_rand --methods mmap,pread --out bench/plots/io_compare.png`

如果你想一键完成“启动 server → 灌数据 → 触发 BGSAVE/BGREWRITEAOF → 只测随机读 → 自动出图”，用：

`chmod +x bench/run_persist_io_compare.sh && WORKLOADS=read_rand ./bench/run_persist_io_compare.sh`

输出：
- `bench/plots/io_compare.png`

