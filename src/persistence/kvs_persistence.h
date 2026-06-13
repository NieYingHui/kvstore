#ifndef _KVS_PERSISTENCE_H_
#define _KVS_PERSISTENCE_H_
/*
 * 持久化总入口（RDB + AOF）。
 *
 * - 写命令成功后调用 kvs_persistence_on_write_*：更新 dirty 并按 save 规则触发后台 BGSAVE。
 * - AOF 的追加写在 AOF 模块中完成（kvs_aof_append_argv）。
 */

int kvs_persistence_init(void);
int kvs_persistence_recover(void);

/*
 * 写命令成功后调用（save 规则，触发后台 BGSAVE）。
 * - 文本协议：传入原始命令行（仅用于统计/兼容；不会写入 WAL 文件）
 * - RESP：传入 argv
 */
int kvs_persistence_on_write_cmdline(const char *cmdline);
int kvs_persistence_on_write_argv(int argc, const char * const *argv);

/*
 * 手动触发后台 RDB 保存（BGSAVE 的一个子集）。
 * 返回：0=成功启动，1=正在进行中，-1=失败。
 */
int kvs_persistence_bgsave(void);

/*
 * 生成/加载 RDB 文件：用于手动/后台快照的落盘与加载。
 * - rdb_save_to_file: 生成 RDB 文件
 * - rdb_load_from_file: 从 RDB 文件加载到内存
 */
int kvs_persistence_rdb_save_to_file(const char *path);
int kvs_persistence_rdb_load_from_file(const char *path);
void kvs_persistence_close(void);


#endif