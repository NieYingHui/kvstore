#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#include "command.h"
#include "engine.h"
#include "../network/server.h"
#include "persistence/kvs_aof.h"
#include "persistence/kvs_persistence.h"

// 将源字符串（src）中的小写字母转换为大写字母，然后复制到目标字符串（dst）中
static void kvs_upper_copy(char *dst, size_t dstsz, const char *src) {
	if (!dst || dstsz == 0) return;
	if (!src) {
		dst[0] = '\0';
		return;
	}
	size_t n = strlen(src);
	if (n >= dstsz) n = dstsz - 1;
	for (size_t i = 0; i < n; ++i) {
		unsigned char ch = (unsigned char)src[i];
		if (ch >= 'a' && ch <= 'z') ch = (unsigned char)(ch - 'a' + 'A');
		dst[i] = (char)ch;
	}
	dst[n] = '\0';
}

// 向一个缓冲区中追加格式化字符串
/*
buf: 目标缓冲区
cap: 缓冲区容量
pos: 当前写入位置指针
fmt: 格式化字符串
...: 可变参数
返回值: 0 成功，-1 失败
*/
static int kvs_buf_append(char *buf, int cap, int *pos, const char *fmt, ...) {
	if (!buf || cap <= 0 || !pos || !fmt) return -1;
	if (*pos < 0 || *pos >= cap) return -1;
	va_list ap; // 使用va_list和vsnprintf函数处理可变参数
	va_start(ap, fmt);
	int n = vsnprintf(buf + *pos, (size_t)(cap - *pos), fmt, ap);
	va_end(ap);
	if (n < 0) return -1;
	if (n >= cap - *pos) return -1;
	*pos += n;
	return 0;
}

static int kvs_buf_append_bulk(char *buf, int cap, int *pos, const char *s) { // 用于在缓冲区中批量追加数据
	if (!s) {
		return kvs_buf_append(buf, cap, pos, "$-1\r\n");
	}
	int len = (int)strlen(s);
	if (kvs_buf_append(buf, cap, pos, "$%d\r\n", len) != 0) return -1;
	if (len > 0) {
		if (*pos + len + 2 >= cap) return -1;
		memcpy(buf + *pos, s, (size_t)len);
		*pos += len;
	}
	if (kvs_buf_append(buf, cap, pos, "\r\n") != 0) return -1; // 写入终止符
	return 0;
}

static int kvs_reply_err_arity(const char *cmd, char *response_buf) { // 命令参数数量错误时的错误响应消息
	if (!cmd) cmd = "";
	return snprintf(response_buf, BUFFER_LENGTH,
		"-ERR wrong number of arguments for '%s' command\r\n", cmd);
}

static int kvs_reply_simple(const char *s, char *response_buf) { // 用于构建一个简单的响应字符串
	if (!s) s = "";
	return snprintf(response_buf, BUFFER_LENGTH, "+%s\r\n", s);
}

static int kvs_reply_int(long v, char *response_buf) { // 将一个长整型(long)值格式化后存入响应缓冲区
	return snprintf(response_buf, BUFFER_LENGTH, ":%ld\r\n", v);
}

static int kvs_reply_bulk(const char *s, char *response_buf) { // 生成符合Redis协议(RESP)的批量字符串回复格式
	if (!s) {
		return snprintf(response_buf, BUFFER_LENGTH, "$-1\r\n");
	}
	int len = (int)strlen(s);
	return snprintf(response_buf, BUFFER_LENGTH, "$%d\r\n%.*s\r\n", len, len, s);
}

/*
 * 各命令实现：直接搬运自原 protocol.c 中对应分支，
 * 保持返回内容与写操作标记完全一致。
 */

static int kvs_cmd_set(int argc, char **argv, char *response_buf, int *is_write_cmd, int is_resp) {
	if (argc != 3) {
		return is_resp ? kvs_reply_err_arity("set", response_buf)
		               : snprintf(response_buf, BUFFER_LENGTH, "NO EXIST\r\n");
	}

	/* SET 覆盖写；底层 set() 若检测已存在会返回 >0。 */
	int ret = kvs_array_set(&global_array, argv[1], argv[2]);
	if (ret > 0) {
		ret = kvs_array_mod(&global_array, argv[1], argv[2]); // 覆盖写
	}
	if (ret == 0) {
		*is_write_cmd = 1;
		return is_resp ? kvs_reply_simple("OK", response_buf)
		               : snprintf(response_buf, BUFFER_LENGTH, "OK\r\n");
	}
	return is_resp ? snprintf(response_buf, BUFFER_LENGTH, "-ERR SET failed\r\n")
	               : snprintf(response_buf, BUFFER_LENGTH, "NO EXIST\r\n");
}

static int kvs_cmd_get(int argc, char **argv, char *response_buf, int *is_write_cmd, int is_resp) {
	(void)is_write_cmd;
	if (argc != 2) {
		return is_resp ? kvs_reply_err_arity("get", response_buf)
		               : snprintf(response_buf, BUFFER_LENGTH, "NO EXIST\r\n");
	}
	char *val = kvs_array_get(&global_array, argv[1]);
	if (!is_resp) {
		if (!val) return snprintf(response_buf, BUFFER_LENGTH, "NO EXIST\r\n");
		return snprintf(response_buf, BUFFER_LENGTH, "%s\r\n", val);
	}
	return kvs_reply_bulk(val, response_buf);
}

static int kvs_cmd_del(int argc, char **argv, char *response_buf, int *is_write_cmd, int is_resp) {
	if (argc < 2) {
		return is_resp ? kvs_reply_err_arity("del", response_buf)
		               : snprintf(response_buf, BUFFER_LENGTH, "NO EXIST\r\n");
	}

	long deleted = 0;
	for (int i = 1; i < argc; ++i) {
		if (!argv[i]) continue;
		int r = kvs_array_del(&global_array, argv[i]);
		if (r == 0) deleted++;
	}
	if (deleted > 0) *is_write_cmd = 1;
	if (!is_resp) {
		return (deleted > 0)
			? snprintf(response_buf, BUFFER_LENGTH, "OK\r\n")
			: snprintf(response_buf, BUFFER_LENGTH, "NO EXIST\r\n");
	}
	return kvs_reply_int(deleted, response_buf);
}

static int kvs_cmd_mod(int argc, char **argv, char *response_buf, int *is_write_cmd, int is_resp) {
	if (argc != 3) {
		return is_resp ? kvs_reply_err_arity("mod", response_buf)
		               : snprintf(response_buf, BUFFER_LENGTH, "NO EXIST\r\n");
	}
	int ret = kvs_array_mod(&global_array, argv[1], argv[2]);
	if (ret == 0) {
		*is_write_cmd = 1;
		return is_resp ? kvs_reply_simple("OK", response_buf)
		               : snprintf(response_buf, BUFFER_LENGTH, "OK\r\n");
	}
	return is_resp ? snprintf(response_buf, BUFFER_LENGTH, "$-1\r\n")
	               : snprintf(response_buf, BUFFER_LENGTH, "NO EXIST\r\n");
}

static int kvs_cmd_exist(int argc, char **argv, char *response_buf, int *is_write_cmd, int is_resp) {
	(void)is_write_cmd;
	if (argc < 2) {
		return is_resp ? kvs_reply_err_arity("exists", response_buf)
		               : snprintf(response_buf, BUFFER_LENGTH, "NO EXIST\r\n");
	}
	long exists = 0;
	for (int i = 1; i < argc; ++i) {
		if (!argv[i]) continue;
		int ret = kvs_array_exist(&global_array, argv[i]);
		if (ret == 0) exists++;
	}
	if (!is_resp) {
		return (exists > 0)
			? snprintf(response_buf, BUFFER_LENGTH, "EXIST\r\n")
			: snprintf(response_buf, BUFFER_LENGTH, "NO EXIST\r\n");
	}
	return kvs_reply_int(exists, response_buf);
}

static int kvs_cmd_exists(int argc, char **argv, char *response_buf, int *is_write_cmd, int is_resp) {
	/* EXISTS 作为兼容别名，复用 EXIST 实现 */
	return kvs_cmd_exist(argc, argv, response_buf, is_write_cmd, is_resp);
}

// ping - 服务器返回 "PONG"
// ping message - 服务器返回 "message"
// ping arg1 arg2 ... - 返回参数数量错误
static int kvs_cmd_ping(int argc, char **argv, char *response_buf, int *is_write_cmd, int is_resp) {
	(void)is_write_cmd;
	if (argc == 1) {
		return is_resp ? kvs_reply_simple("PONG", response_buf)
		               : snprintf(response_buf, BUFFER_LENGTH, "PONG\r\n");
	}
	if (argc == 2) {
		return is_resp ? kvs_reply_bulk(argv[1], response_buf)
		               : snprintf(response_buf, BUFFER_LENGTH, "%s\r\n", argv[1]);
	}
	return is_resp ? kvs_reply_err_arity("ping", response_buf)
	               : snprintf(response_buf, BUFFER_LENGTH, "ERR\r\n");
}

static int kvs_cmd_echo(int argc, char **argv, char *response_buf, int *is_write_cmd, int is_resp) {
	(void)is_write_cmd;
	if (argc != 2) {
		return is_resp ? kvs_reply_err_arity("echo", response_buf)
		               : snprintf(response_buf, BUFFER_LENGTH, "ERR\r\n");
	}
	return is_resp ? kvs_reply_bulk(argv[1], response_buf)
	               : snprintf(response_buf, BUFFER_LENGTH, "%s\r\n", argv[1]);
}

static int kvs_cmd_quit(int argc, char **argv, char *response_buf, int *is_write_cmd, int is_resp) {
	(void)argc;
	(void)argv;
	(void)is_write_cmd;
	/* 连接关闭由客户端发起即可；这里按 Redis 行为返回 OK。 */
	return is_resp ? kvs_reply_simple("OK", response_buf)
	               : snprintf(response_buf, BUFFER_LENGTH, "OK\r\n");
}

static int kvs_cmd_command(int argc, char **argv, char *response_buf, int *is_write_cmd, int is_resp) {
	(void)argc;
	(void)argv;
	(void)is_write_cmd;
	/* 最小兼容：返回空数组，满足 redis-cli/工具探测。 */
	return is_resp ? snprintf(response_buf, BUFFER_LENGTH, "*0\r\n")
	               : snprintf(response_buf, BUFFER_LENGTH, "OK\r\n");
}

static int kvs_cmd_client(int argc, char **argv, char *response_buf, int *is_write_cmd, int is_resp) {
	(void)is_write_cmd;
	/* redis-cli 可能会发：CLIENT SETINFO LIB-NAME xxx / LIB-VER xxx */
	if (argc >= 2 && argv[1]) {
		char subcmd[32];
		kvs_upper_copy(subcmd, sizeof(subcmd), argv[1]);
		if (strcmp(subcmd, "SETINFO") == 0) {
			return is_resp ? kvs_reply_simple("OK", response_buf)
			               : snprintf(response_buf, BUFFER_LENGTH, "OK\r\n");
		}
	}
	return is_resp ? snprintf(response_buf, BUFFER_LENGTH, "-ERR unsupported CLIENT subcommand\r\n")
	               : snprintf(response_buf, BUFFER_LENGTH, "ERR\r\n");
}

static int kvs_cmd_info(int argc, char **argv, char *response_buf, int *is_write_cmd, int is_resp) {
	(void)argc;
	(void)argv;
	(void)is_write_cmd;
	const char *payload = "# Server\r\nredis_version:kvstore\r\n";
	return is_resp ? kvs_reply_bulk(payload, response_buf)
	               : snprintf(response_buf, BUFFER_LENGTH, "%s\r\n", payload);
}

static int kvs_cmd_hello(int argc, char **argv, char *response_buf, int *is_write_cmd, int is_resp) {
	(void)is_write_cmd;
	if (!is_resp) {
		return snprintf(response_buf, BUFFER_LENGTH, "OK\r\n");
	}
	/* 最小 HELLO：让新版 redis-cli 能顺利握手。 */
	int proto = 2;
	if (argc >= 2 && argv[1]) {
		proto = atoi(argv[1]);
		if (proto != 2 && proto != 3) proto = 2;
	}
	/* RESP3 map（%）在 RESP2 客户端里也会被当作未知类型；但 redis-cli / redis-benchmark 可解析。 */
	return snprintf(response_buf, BUFFER_LENGTH,
		"%%7\r\n"
		"+server\r\n+kvstore\r\n"
		"+version\r\n+0.0\r\n"
		"+proto\r\n:%d\r\n"
		"+id\r\n:1\r\n"
		"+mode\r\n+standalone\r\n"
		"+role\r\n+master\r\n"
		"+modules\r\n*0\r\n",
		proto);
}

static const char *kvs_aof_fsync_to_str(int v) {
	switch (v) {
		case KVS_AOF_FSYNC_ALWAYS: return "always";
		case KVS_AOF_FSYNC_EVERYSEC: return "everysec";
		case KVS_AOF_FSYNC_NO: return "no";
		default: return "everysec";
	}
}

static void kvs_build_save_string(char *dst, size_t cap) { // 构建一个保存规则的字符串表示
	if (!dst || cap == 0) return;
	dst[0] = '\0';
	int pos = 0;
	for (int i = 0; i < kvs_server.config.save_rules_count; ++i) {
		char part[64];
		int n = snprintf(part, sizeof(part), "%d %d", kvs_server.config.save_rules[i].seconds,
		                 kvs_server.config.save_rules[i].changes);
		if (n <= 0) continue;
		if (i > 0) {
			if ((size_t)pos + 1 >= cap) break;
			dst[pos++] = ' ';
			dst[pos] = '\0';
		}
		if ((size_t)pos + (size_t)n >= cap) break;
		memcpy(dst + pos, part, (size_t)n);
		pos += n;
		dst[pos] = '\0';
	}
}

static int kvs_cmd_config(int argc, char **argv, char *response_buf, int *is_write_cmd, int is_resp) {
	(void)is_write_cmd;
	if (!is_resp) {
		/* 文本协议下不做严格兼容 */
		return snprintf(response_buf, BUFFER_LENGTH, "OK\r\n");
	}
	if (argc < 2 || !argv[1]) {
		return kvs_reply_err_arity("config", response_buf);
	}

	char subcmd[32];
	kvs_upper_copy(subcmd, sizeof(subcmd), argv[1]);

	if (strcmp(subcmd, "RESETSTAT") == 0) {
		return kvs_reply_simple("OK", response_buf);
	}

	if (strcmp(subcmd, "GET") != 0) {
		return snprintf(response_buf, BUFFER_LENGTH, "-ERR unsupported CONFIG subcommand\r\n");
	}
	if (argc != 3 || !argv[2]) {
		return kvs_reply_err_arity("config", response_buf);
	}

	char key[64];
	kvs_upper_copy(key, sizeof(key), argv[2]);

	/* 构建键值对数组。 */
	char savebuf[256];
	kvs_build_save_string(savebuf, sizeof(savebuf));

	struct kvpair { const char *k; const char *v; } pairs[8];
	int npairs = 0;

	const char *appendonly = (kvs_server.config.enable_persistence && kvs_server.config.enable_aof) ? "yes" : "no";
	const char *appendfsync = kvs_aof_fsync_to_str(kvs_server.config.aof_fsync);
	const char *dir = kvs_server.config.dir;
	const char *appendfilename = kvs_server.config.aof_path;
	const char *dbfilename = kvs_server.config.rdb_path;

	int want_all = (strcmp(key, "*") == 0);
	if (want_all || strcmp(key, "APPENDONLY") == 0) {
		pairs[npairs++] = (struct kvpair){"appendonly", appendonly};
	}
	if (want_all || strcmp(key, "APPENDFILENAME") == 0) {
		pairs[npairs++] = (struct kvpair){"appendfilename", appendfilename};
	}
	if (want_all || strcmp(key, "APPENDFSYNC") == 0) {
		pairs[npairs++] = (struct kvpair){"appendfsync", appendfsync};
	}
	if (want_all || strcmp(key, "DIR") == 0) {
		pairs[npairs++] = (struct kvpair){"dir", dir};
	}
	if (want_all || strcmp(key, "DBFILENAME") == 0) {
		pairs[npairs++] = (struct kvpair){"dbfilename", dbfilename};
	}
	if (want_all || strcmp(key, "SAVE") == 0) {
		pairs[npairs++] = (struct kvpair){"save", savebuf};
	}

	int pos = 0;
	if (kvs_buf_append(response_buf, BUFFER_LENGTH, &pos, "*%d\r\n", npairs * 2) != 0) {
		return snprintf(response_buf, BUFFER_LENGTH, "-ERR CONFIG reply too large\r\n");
	}
	for (int i = 0; i < npairs; ++i) {
		if (kvs_buf_append_bulk(response_buf, BUFFER_LENGTH, &pos, pairs[i].k) != 0) {
			return snprintf(response_buf, BUFFER_LENGTH, "-ERR CONFIG reply too large\r\n");
		}
		if (kvs_buf_append_bulk(response_buf, BUFFER_LENGTH, &pos, pairs[i].v) != 0) {
			return snprintf(response_buf, BUFFER_LENGTH, "-ERR CONFIG reply too large\r\n");
		}
	}
	return pos;
}

static int kvs_cmd_hset(int argc, char **argv, char *response_buf, int *is_write_cmd, int is_resp) {
	if (argc != 3) {
		return is_resp ? kvs_reply_err_arity("hset", response_buf)
		               : snprintf(response_buf, BUFFER_LENGTH, "NO EXIST\r\n");
	}

	int ret = kvs_hash_set(&global_hash, argv[1], argv[2]);
	int created = 0;
	if (ret > 0) {
		/* 覆盖写：已存在则改写 */
		ret = kvs_hash_mod(&global_hash, argv[1], argv[2]);
		created = 0;
	} else if (ret == 0) {
		created = 1;
	}

	if (ret == 0) {
		*is_write_cmd = 1;
		if (!is_resp) return snprintf(response_buf, BUFFER_LENGTH, "OK\r\n");
		/* Redis HSET：新建返回 1，更新返回 0 */
		return kvs_reply_int(created ? 1 : 0, response_buf);
	}

	return is_resp ? snprintf(response_buf, BUFFER_LENGTH, "-ERR HSET failed\r\n")
	               : snprintf(response_buf, BUFFER_LENGTH, "NO EXIST\r\n");
}

static int kvs_cmd_hget(int argc, char **argv, char *response_buf, int *is_write_cmd, int is_resp) {
	(void)is_write_cmd;
	if (argc != 2) {
		return is_resp ? kvs_reply_err_arity("hget", response_buf)
		               : snprintf(response_buf, BUFFER_LENGTH, "NO EXIST\r\n");
	}
	char *val = kvs_hash_get(&global_hash, argv[1]);
	if (!is_resp) {
		if (!val) return snprintf(response_buf, BUFFER_LENGTH, "NO EXIST\r\n");
		return snprintf(response_buf, BUFFER_LENGTH, "%s\r\n", val);
	}
	return kvs_reply_bulk(val, response_buf);
}

static int kvs_cmd_hdel(int argc, char **argv, char *response_buf, int *is_write_cmd, int is_resp) {
	if (argc < 2) {
		return is_resp ? kvs_reply_err_arity("hdel", response_buf)
		               : snprintf(response_buf, BUFFER_LENGTH, "NO EXIST\r\n");
	}
	long deleted = 0;
	for (int i = 1; i < argc; ++i) {
		int ret = kvs_hash_del(&global_hash, argv[i]);
		if (ret == 0) deleted++;
	}
	if (deleted > 0) *is_write_cmd = 1;
	if (!is_resp) {
		return (deleted > 0)
			? snprintf(response_buf, BUFFER_LENGTH, "OK\r\n")
			: snprintf(response_buf, BUFFER_LENGTH, "NO EXIST\r\n");
	}
	return kvs_reply_int(deleted, response_buf);
}

static int kvs_cmd_hexists(int argc, char **argv, char *response_buf, int *is_write_cmd, int is_resp) {
	(void)is_write_cmd;
	if (argc < 2) {
		return is_resp ? kvs_reply_err_arity("hexists", response_buf)
		               : snprintf(response_buf, BUFFER_LENGTH, "NO EXIST\r\n");
	}
	long exists = 0;
	for (int i = 1; i < argc; ++i) {
		int ret = kvs_hash_exist(&global_hash, argv[i]);
		if (ret == 0) exists++;
	}
	if (!is_resp) {
		return (exists > 0)
			? snprintf(response_buf, BUFFER_LENGTH, "HEXIST\r\n")
			: snprintf(response_buf, BUFFER_LENGTH, "NO EXIST\r\n");
	}
	return kvs_reply_int(exists, response_buf);
}

static int kvs_cmd_hmod(int argc, char **argv, char *response_buf, int *is_write_cmd, int is_resp) {
	if (argc != 3) {
		return is_resp ? kvs_reply_err_arity("hmod", response_buf)
		               : snprintf(response_buf, BUFFER_LENGTH, "NO EXIST\r\n");
	}
	int ret = kvs_hash_mod(&global_hash, argv[1], argv[2]);
	if (ret == 0) {
		*is_write_cmd = 1;
		return is_resp ? kvs_reply_simple("OK", response_buf)
		               : snprintf(response_buf, BUFFER_LENGTH, "OK\r\n");
	}
	return is_resp ? snprintf(response_buf, BUFFER_LENGTH, "$-1\r\n")
	               : snprintf(response_buf, BUFFER_LENGTH, "NO EXIST\r\n");
}

static int kvs_cmd_rset(int argc, char **argv, char *response_buf, int *is_write_cmd, int is_resp) {
	if (argc != 3) {
		return is_resp ? kvs_reply_err_arity("rset", response_buf)
		               : snprintf(response_buf, BUFFER_LENGTH, "NO EXIST\r\n");
	}
	int ret = kvs_rbtree_set(&global_rbtree, argv[1], argv[2]);
	if (ret > 0) ret = kvs_rbtree_mod(&global_rbtree, argv[1], argv[2]);
	if (ret == 0) {
		*is_write_cmd = 1;
		return is_resp ? kvs_reply_simple("OK", response_buf)
		               : snprintf(response_buf, BUFFER_LENGTH, "OK\r\n");
	}
	return is_resp ? snprintf(response_buf, BUFFER_LENGTH, "-ERR RSET failed\r\n")
	               : snprintf(response_buf, BUFFER_LENGTH, "NO EXIST\r\n");
}

static int kvs_cmd_rget(int argc, char **argv, char *response_buf, int *is_write_cmd, int is_resp) {
	(void)is_write_cmd;
	if (argc != 2) {
		return is_resp ? kvs_reply_err_arity("rget", response_buf)
		               : snprintf(response_buf, BUFFER_LENGTH, "NO EXIST\r\n");
	}
	char *val = kvs_rbtree_get(&global_rbtree, argv[1]);
	if (!is_resp) {
		if (!val) return snprintf(response_buf, BUFFER_LENGTH, "NO EXIST\r\n");
		return snprintf(response_buf, BUFFER_LENGTH, "%s\r\n", val);
	}
	return kvs_reply_bulk(val, response_buf);
}

static int kvs_cmd_rdel(int argc, char **argv, char *response_buf, int *is_write_cmd, int is_resp) {
	if (argc < 2) {
		return is_resp ? kvs_reply_err_arity("rdel", response_buf)
		               : snprintf(response_buf, BUFFER_LENGTH, "NO EXIST\r\n");
	}
	long deleted = 0;
	for (int i = 1; i < argc; ++i) {
		int ret = kvs_rbtree_del(&global_rbtree, argv[i]);
		if (ret == 0) deleted++;
	}
	if (deleted > 0) *is_write_cmd = 1;
	if (!is_resp) {
		return (deleted > 0)
			? snprintf(response_buf, BUFFER_LENGTH, "OK\r\n")
			: snprintf(response_buf, BUFFER_LENGTH, "NO EXIST\r\n");
	}
	return kvs_reply_int(deleted, response_buf);
}

static int kvs_cmd_rexists(int argc, char **argv, char *response_buf, int *is_write_cmd, int is_resp) {
	(void)is_write_cmd;
	if (argc < 2) {
		return is_resp ? kvs_reply_err_arity("rexists", response_buf)
		               : snprintf(response_buf, BUFFER_LENGTH, "NO EXIST\r\n");
	}
	long exists = 0;
	for (int i = 1; i < argc; ++i) {
		int ret = kvs_rbtree_exist(&global_rbtree, argv[i]);
		if (ret == 0) exists++;
	}
	if (!is_resp) {
		return (exists > 0)
			? snprintf(response_buf, BUFFER_LENGTH, "REXIST\r\n")
			: snprintf(response_buf, BUFFER_LENGTH, "NO EXIST\r\n");
	}
	return kvs_reply_int(exists, response_buf);
}

static int kvs_cmd_rmod(int argc, char **argv, char *response_buf, int *is_write_cmd, int is_resp) {
	if (argc != 3) {
		return is_resp ? kvs_reply_err_arity("rmod", response_buf)
		               : snprintf(response_buf, BUFFER_LENGTH, "NO EXIST\r\n");
	}
	int ret = kvs_rbtree_mod(&global_rbtree, argv[1], argv[2]);
	if (ret == 0) {
		*is_write_cmd = 1;
		return is_resp ? kvs_reply_simple("OK", response_buf)
		               : snprintf(response_buf, BUFFER_LENGTH, "OK\r\n");
	}
	return is_resp ? snprintf(response_buf, BUFFER_LENGTH, "$-1\r\n")
	               : snprintf(response_buf, BUFFER_LENGTH, "NO EXIST\r\n");
}

static int kvs_cmd_lset(int argc, char **argv, char *response_buf, int *is_write_cmd, int is_resp) {
	if (argc != 3) {
		return is_resp ? kvs_reply_err_arity("lset", response_buf)
		               : snprintf(response_buf, BUFFER_LENGTH, "NO EXIST\r\n");
	}
	int ret = kvs_skiplist_set(&global_skiplist, argv[1], argv[2]);
	if (ret > 0) ret = kvs_skiplist_mod(&global_skiplist, argv[1], argv[2]);
	if (ret == 0) {
		*is_write_cmd = 1;
		return is_resp ? kvs_reply_simple("OK", response_buf)
		               : snprintf(response_buf, BUFFER_LENGTH, "OK\r\n");
	}
	return is_resp ? snprintf(response_buf, BUFFER_LENGTH, "-ERR LSET failed\r\n")
	               : snprintf(response_buf, BUFFER_LENGTH, "NO EXIST\r\n");
}

static int kvs_cmd_lget(int argc, char **argv, char *response_buf, int *is_write_cmd, int is_resp) {
	(void)is_write_cmd;
	if (argc != 2) {
		return is_resp ? kvs_reply_err_arity("lget", response_buf)
		               : snprintf(response_buf, BUFFER_LENGTH, "NO EXIST\r\n");
	}
	char *val = kvs_skiplist_get(&global_skiplist, argv[1]);
	if (!is_resp) {
		if (!val) return snprintf(response_buf, BUFFER_LENGTH, "NO EXIST\r\n");
		return snprintf(response_buf, BUFFER_LENGTH, "%s\r\n", val);
	}
	return kvs_reply_bulk(val, response_buf);
}

static int kvs_cmd_ldel(int argc, char **argv, char *response_buf, int *is_write_cmd, int is_resp) {
	if (argc < 2) {
		return is_resp ? kvs_reply_err_arity("ldel", response_buf)
		               : snprintf(response_buf, BUFFER_LENGTH, "NO EXIST\r\n");
	}
	long deleted = 0;
	for (int i = 1; i < argc; ++i) {
		int ret = kvs_skiplist_del(&global_skiplist, argv[i]);
		if (ret == 0) deleted++;
	}
	if (deleted > 0) *is_write_cmd = 1;
	if (!is_resp) {
		return (deleted > 0)
			? snprintf(response_buf, BUFFER_LENGTH, "OK\r\n")
			: snprintf(response_buf, BUFFER_LENGTH, "NO EXIST\r\n");
	}
	return kvs_reply_int(deleted, response_buf);
}

static int kvs_cmd_lexists(int argc, char **argv, char *response_buf, int *is_write_cmd, int is_resp) {
	(void)is_write_cmd;
	if (argc < 2) {
		return is_resp ? kvs_reply_err_arity("lexists", response_buf)
		               : snprintf(response_buf, BUFFER_LENGTH, "NO EXIST\r\n");
	}
	long exists = 0;
	for (int i = 1; i < argc; ++i) {
		int ret = kvs_skiplist_exist(&global_skiplist, argv[i]);
		if (ret == 0) exists++;
	}
	if (!is_resp) {
		return (exists > 0)
			? snprintf(response_buf, BUFFER_LENGTH, "LEXIST\r\n")
			: snprintf(response_buf, BUFFER_LENGTH, "NO EXIST\r\n");
	}
	return kvs_reply_int(exists, response_buf);
}

static int kvs_cmd_lmod(int argc, char **argv, char *response_buf, int *is_write_cmd, int is_resp) {
	if (argc != 3) {
		return is_resp ? kvs_reply_err_arity("lmod", response_buf)
		               : snprintf(response_buf, BUFFER_LENGTH, "NO EXIST\r\n");
	}
	int ret = kvs_skiplist_mod(&global_skiplist, argv[1], argv[2]);
	if (ret == 0) {
		*is_write_cmd = 1;
		return is_resp ? kvs_reply_simple("OK", response_buf)
		               : snprintf(response_buf, BUFFER_LENGTH, "OK\r\n");
	}
	return is_resp ? snprintf(response_buf, BUFFER_LENGTH, "$-1\r\n")
	               : snprintf(response_buf, BUFFER_LENGTH, "NO EXIST\r\n");
}

static int kvs_cmd_bgrewriteaof(int argc, char **argv, char *response_buf, int *is_write_cmd, int is_resp) {
	(void)argc;
	(void)argv;
	(void)is_write_cmd;
	if (!kvs_server.config.enable_persistence || !kvs_server.config.enable_aof) {
		return is_resp ? snprintf(response_buf, BUFFER_LENGTH, "-ERR AOF is disabled\r\n")
		               : snprintf(response_buf, BUFFER_LENGTH, "ERR\r\n");
	}
	int r = kvs_aof_bgrewriteaof_start();  //开始重写AOF文件
	if (r == 1) {
		return snprintf(response_buf, BUFFER_LENGTH, "+BUSY\r\n");
	}
	if (r != 0) {
		return snprintf(response_buf, BUFFER_LENGTH, "-ERR BGREWRITEAOF failed\r\n");
	}
	return is_resp ? snprintf(response_buf, BUFFER_LENGTH, "+OK\r\n")
	               : snprintf(response_buf, BUFFER_LENGTH, "OK\r\n");
}

static int kvs_cmd_bgsave(int argc, char **argv, char *response_buf, int *is_write_cmd, int is_resp) {
	(void)argc;
	(void)argv;
	(void)is_write_cmd;

	if (!kvs_server.config.enable_persistence) {
		return is_resp ? snprintf(response_buf, BUFFER_LENGTH, "-ERR persistence is disabled\r\n")
		               : snprintf(response_buf, BUFFER_LENGTH, "ERR\r\n");
	}

	int r = kvs_persistence_bgsave();
	if (r == 1) {
		return snprintf(response_buf, BUFFER_LENGTH, "+BUSY\r\n");
	}
	if (r != 0) {
		return snprintf(response_buf, BUFFER_LENGTH, "-ERR BGSAVE failed\r\n");
	}
	return is_resp ? snprintf(response_buf, BUFFER_LENGTH, "+OK\r\n")
	               : snprintf(response_buf, BUFFER_LENGTH, "OK\r\n");
}

static const kvsCommand kvsCommandTable[] = {
	{ "PING",   0, 0, kvs_cmd_ping },
	{ "ECHO",   0, 0, kvs_cmd_echo },
	{ "QUIT",   0, 0, kvs_cmd_quit },
	{ "HELLO",  0, 0, kvs_cmd_hello },
	{ "COMMAND",0, 0, kvs_cmd_command },
	{ "CLIENT", 0, 0, kvs_cmd_client },
	{ "INFO",   0, 0, kvs_cmd_info },
	{ "CONFIG", 0, 0, kvs_cmd_config },

	{ "SET",    0, 0, kvs_cmd_set },
	{ "GET",    0, 0, kvs_cmd_get },
	{ "DEL",    0, 0, kvs_cmd_del },
	{ "MOD",    0, 0, kvs_cmd_mod },
	{ "EXIST",  0, 0, kvs_cmd_exist },
	{ "EXISTS", 0, 0, kvs_cmd_exists },
	{ "HSET",   0, 0, kvs_cmd_hset },
	{ "HGET",   0, 0, kvs_cmd_hget },
	{ "HDEL",   0, 0, kvs_cmd_hdel },
	{ "HMOD",   0, 0, kvs_cmd_hmod },
	{ "HEXISTS",0, 0, kvs_cmd_hexists },

	{ "RSET",   0, 0, kvs_cmd_rset },
	{ "RGET",   0, 0, kvs_cmd_rget },
	{ "RDEL",   0, 0, kvs_cmd_rdel },
	{ "RMOD",   0, 0, kvs_cmd_rmod },
	{ "REXISTS",0, 0, kvs_cmd_rexists },

	{ "LSET",   0, 0, kvs_cmd_lset },
	{ "LGET",   0, 0, kvs_cmd_lget },
	{ "LDEL",   0, 0, kvs_cmd_ldel },
	{ "LMOD",   0, 0, kvs_cmd_lmod },
	{ "LEXISTS",0, 0, kvs_cmd_lexists },

	/* 兼容旧命令名（保留） */
	{ "HEXIST", 0, 0, kvs_cmd_hexists },
	{ "REXIST", 0, 0, kvs_cmd_rexists },
	{ "LEXIST", 0, 0, kvs_cmd_lexists },
	{ "BGSAVE", 0, 0, kvs_cmd_bgsave },
	{ "BGREWRITEAOF", 0, 0, kvs_cmd_bgrewriteaof },
};

const kvsCommand *kvs_lookup_command(const char *name) {
	if (!name) return NULL;
	size_t i;
	for (i = 0; i < sizeof(kvsCommandTable) / sizeof(kvsCommandTable[0]); ++i) {
		if (strcmp(name, kvsCommandTable[i].name) == 0) {
			return &kvsCommandTable[i];
		}
	}
	return NULL;
}
