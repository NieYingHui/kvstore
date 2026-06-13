#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#include "protocol.h"
#include "engine.h"
#include "command.h"
#include "../network/server.h"
#include "persistence/kvs_persistence.h"
#include "persistence/kvs_aof.h"
#include "client.h"
#include "ebpf/kvs_ebpf_hook.h"

static void kvs_trim_trailing(char *s) { // 删除字符串末尾空白字符的函数
    size_t len;
    if (!s) return;
    len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' || s[len - 1] == ' ' || s[len - 1] == '\t')) {
        s[--len] = '\0';
    }
}

static void kvs_str_to_upper(char *s) { // 将字符串转换为大写
    if (!s) return;
    for (; *s; ++s) {
        *s = (char)toupper((unsigned char)*s);
    }
}

// 从文本流中提取长整型数值
// "123\nrest of text"  p 指向 '1' end 指向文本末尾 *out = 123, *line_end_out 指向 '\n'
static int kvs_parse_long_field(const char *p, const char *end, long *out, const char **line_end_out) {
    if (!p || !end || !out || !line_end_out) return -1;
    const char *nl = memchr(p, '\n', (size_t)(end - p));
    if (!nl) return 0; /* 不完整 */

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
    /* 允许 q 停在 \r 或直接停在 \n 前 */
    if (q < nl && *q != '\r') {
        return -1;
    }

    *out = v * sign;
    *line_end_out = nl;
    return 1;
}

int kvs_spilt_token(char *msg, char *tokens[]) {
    if (!msg || !tokens) return -1;
    int count = 0;
    char *saveptr = NULL;
    char *p = strtok_r(msg, " ", &saveptr);
    while (p && count < KVS_MAX_TOKENS) {
        tokens[count++] = p;
        p = strtok_r(NULL, " ", &saveptr);
    }
    return count;
}

// 解析RESP格式的消息
static int kvs_parse_resp(char *buf, int length, char *tokens[], int *consumed) {
// buf *2\r\n$3\r\nfoo\r\n$3\r\nbar\r\n
// length 26

    if (!buf || !tokens || !consumed || length <= 0) return -1;
    if (buf[0] != '*') return -1;

    char *end = buf + length;

    /* 先做一遍无损“完整性检查”，避免半包时修改缓冲导致后续解析失败。 */
    const char *p = buf + 1;
    const char *line_end = NULL;
    long argc = 0;
    int r = kvs_parse_long_field(p, end, &argc, &line_end); // 解析参数个数:2
    if (r == 0) return 0; /* 不完整 */
    if (r < 0) return -1;
    if (argc <= 0 || argc > KVS_MAX_TOKENS) return -1;
    p = line_end + 1;

    const char *arg_starts[KVS_MAX_TOKENS] = {0};
    long arg_lens[KVS_MAX_TOKENS] = {0};

    for (long i = 0; i < argc; ++i) {
        if (p >= end || *p != '$') return -1;
        p++; /* skip '$' */

        long blen = 0;
        r = kvs_parse_long_field(p, end, &blen, &line_end); // 解析第 i 个参数的长度
        if (r == 0) return 0; /* 不完整 */
        if (r < 0) return -1;
        if (blen < 0) return -1; /* 不支持 Null Bulk */

        p = line_end + 1; /* 指向数据起始 */
        if (p + blen + 2 > end) return 0; /* 不完整 */
        if (p[blen] != '\r' || p[blen + 1] != '\n') return -1;

        arg_starts[i] = p;
        arg_lens[i] = blen;
        p = p + blen + 2;
    }

    /* 完整性检查通过：现在才开始“就地截断”和填充 tokens。 */
    for (long i = 0; i < argc; ++i) {
        tokens[i] = (char *)arg_starts[i];
        ((char *)arg_starts[i])[arg_lens[i]] = '\0'; /* 覆盖 '\r'，得到 C 字符串 */
    }
// tokens[0] = "foo"
// tokens[1] = "bar"
    *consumed = (int)(p - buf); // consumed = 21 (整个消息的长度)
    return (int)argc; // 返回参数个数 : 2
}

/*
 * 将旧的文本响应（例如 "OK\r\n" 或 "value\r\n"）包装为 RESP Bulk String：
 *   $<len>\r\n<payload>\r\n
 */
static int kvs_format_resp_bulk(const char *text, int text_len, char *out_buf, int out_cap) {
    if (!out_buf || out_cap <= 0) return 0;
    if (!text || text_len <= 0) {
        /* 空响应按空字符串处理 */
        return snprintf(out_buf, (size_t)out_cap, "$0\r\n\r\n");
    }
    int len = text_len;
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r')) {
        len--;
    }
    if (len < 0) len = 0;
    return snprintf(out_buf, (size_t)out_cap, "$%d\r\n%.*s\r\n", len, len, text);
}

// 判断给定的字符串是否符合RESP协议的原始回复格式
static int kvs_is_raw_resp_reply(const char *s, int len) {
    if (!s || len <= 0) return 0;
    switch (s[0]) {
        case '+':
        case '-':
        case ':':
        case '$':
        case '*':
        case '%': /* RESP3 map */
            return 1;
        default:
            return 0;
    }
}

/* 最近一次协议解析所消费的输入字节数，用于上层做流式拼接。 */
static int g_kvs_last_consumed = 0;

// 可以处理两种协议：文本协议和RESP协议
static int kvs_protocol_internal(char *msg, int length, char *response_buf) { // msg：SET mykey myvalue
// msg = "SET mykey myvalue\r\n"; 简单的文本协议
// length = 18;

// msg = "*3\r\n$3\r\nSET\r\n$5\r\nmykey\r\n$7\r\nmyvalue\r\n"; RESP协议
// length = 33;
    if (!msg || !response_buf || length <= 0) {
        g_kvs_last_consumed = 0;
        return 0;
    }

    char buf[BUFFER_LENGTH];
    if (length >= (int)sizeof(buf)) {
        length = (int)sizeof(buf) - 1;
    }
    memcpy(buf, msg, (size_t)length);
    buf[length] = '\0';

    int total_out = 0;
    char *in = buf;
    int remaining = length; // 剩余的待处理字节数

    while (remaining > 0 && total_out < BUFFER_LENGTH) {
        /* 跳过前导空白和空行 */
        while (remaining > 0 && (*in == '\r' || *in == '\n' || *in == ' ' || *in == '\t')) {
            in++;
            remaining--;
        }
        if (remaining <= 0) break;

        int is_resp = (*in == '*'); // 判断是不是RESP协议
        char *tokens[KVS_MAX_TOKENS] = {0};
        int count = 0;
        int consumed = 0;

        if (is_resp) {
            count = kvs_parse_resp(in, remaining, tokens, &consumed); // 解析出来的tokens和文本协议的一样
            if (count < 0) {
                /* 协议错误：返回错误并丢弃后续数据 */
                int n = snprintf(response_buf + total_out,
                    (size_t)(BUFFER_LENGTH - total_out),
                    "-ERR protocol error\r\n");
                if (n > 0) total_out += n;
                remaining = 0;
                break;
            } else if (count == 0) {
                /* 数据不完整：保留剩余数据，等待下一次输入 */
                break;
            }
        } else {
            /* 文本协议：按行解析一条命令 */
            char *nl = memchr(in, '\n', (size_t)remaining);
            int line_len = nl ? (int)(nl - in + 1) : remaining;
            /* 在 buf 内就地截断一行，避免 tokens 指向临时栈缓冲导致悬空指针 */
            char saved_nl = 0;
            if (nl) {
                saved_nl = *nl;
                *nl = '\0';
                if (nl > in && nl[-1] == '\r') nl[-1] = '\0';
            } else {
                /* 最后一行无换行：保证 NUL 结束 */
                in[line_len] = '\0';
            }

            kvs_trim_trailing(in);
            if (in[0] == '\0') {
                if (nl) *nl = saved_nl;
                in += line_len;
                remaining -= line_len;
                continue;
            }

            count = kvs_spilt_token(in, tokens);
            if (count <= 0) {
                int n = snprintf(response_buf + total_out, (size_t)(BUFFER_LENGTH - total_out), "ERR\r\n");
                if (n > 0) total_out += n;
                if (nl) *nl = saved_nl;
                in += line_len;
                remaining -= line_len;
                continue;
            }
            consumed = line_len;

            if (nl) *nl = saved_nl;
        }

        /* 命令名统一转为大写，支持大小写不敏感 */
        kvs_str_to_upper(tokens[0]);
        const char *cmd = tokens[0];
        int is_write_cmd = 0;

        const kvsCommand *command = kvs_lookup_command(cmd);
        if (!command || !command->proc) {
            if (is_resp) {
                int n = snprintf(response_buf + total_out,
                    (size_t)(BUFFER_LENGTH - total_out),
                    "-ERR unknown command '%s'\r\n", cmd ? cmd : "");
                if (n > 0) total_out += n;
            } else {
                int n = snprintf(response_buf + total_out,
                    (size_t)(BUFFER_LENGTH - total_out),
                    "ERR\r\n");
                if (n > 0) total_out += n;
            }
            in += consumed;
            remaining -= consumed;
            continue;
        }

        /* 每条命令先写入局部缓冲，再统一追加到 response_buf，避免命令实现对缓冲区大小的假设 */
        char inner[BUFFER_LENGTH];
        memset(inner, 0, sizeof(inner));
        int inner_len = command->proc(count, tokens, inner, &is_write_cmd, is_resp);
        if (inner_len < 0) inner_len = 0;

        if (!is_resp) {
            /* 文本协议：直接追加 inner 内容 */
            int copy_len = inner_len;
            if (copy_len > (BUFFER_LENGTH - total_out)) {
                copy_len = BUFFER_LENGTH - total_out;
            }
            if (copy_len > 0) {
                memcpy(response_buf + total_out, inner, (size_t)copy_len);
                total_out += copy_len;
            }

#ifdef PERSISTENCE_ENABLE
            if (is_write_cmd) {
                /* 文本协议直接记录原始行（去掉末尾换行） */
                char wal_line[BUFFER_LENGTH];
                int wlen = consumed;
                if (wlen >= BUFFER_LENGTH) wlen = BUFFER_LENGTH - 1;
                memcpy(wal_line, in, (size_t)wlen);
                wal_line[wlen] = '\0';
                kvs_trim_trailing(wal_line);
                if (wal_line[0] != '\0') {
                    kvs_persistence_on_write_cmdline(wal_line);
                }

                /* AOF：将规范化的 argv 记录为 RESP */
                if (kvs_server.config.enable_persistence && kvs_server.config.enable_aof) {
                    (void)kvs_aof_append_argv(count, (const char * const *)tokens);
                }
            }
#endif

            /* eBPF replication hook: called only after successful writes. */
            if (is_write_cmd) {
                kvs_ebpf_repl_hook(count, tokens, is_resp);
            }

        } else {
            /* RESP 请求：将 inner 包装为 RESP Bulk */
            int out_cap = BUFFER_LENGTH - total_out;
            if (out_cap <= 0) break;

            if (kvs_is_raw_resp_reply(inner, inner_len)) {
                int copy_len = inner_len;
                if (copy_len > out_cap) copy_len = out_cap;
                if (copy_len > 0) {
                    memcpy(response_buf + total_out, inner, (size_t)copy_len);
                    total_out += copy_len;
                }
            } else {
                int written = kvs_format_resp_bulk(inner, inner_len,
                    response_buf + total_out, out_cap);
                if (written < 0) written = 0;
                if (written > out_cap) written = out_cap;
                total_out += written;
            }

#ifdef PERSISTENCE_ENABLE
            if (is_write_cmd) {
                kvs_persistence_on_write_argv(count, (const char * const *)tokens);

                if (kvs_server.config.enable_persistence && kvs_server.config.enable_aof) {
                    (void)kvs_aof_append_argv(count, (const char * const *)tokens);
                }
            }
#endif

            /* eBPF replication hook: called only after successful writes. */
            if (is_write_cmd) {
                kvs_ebpf_repl_hook(count, tokens, is_resp);
            }

        }

        in += consumed;
        remaining -= consumed;
    }

    g_kvs_last_consumed = length - remaining;
    if (g_kvs_last_consumed < 0) g_kvs_last_consumed = 0;
    return total_out;
}

// 流式协议处理器，可以处理RESP协议，特别适合处理大值数据
int kvs_protocol_stream(char *msg, int length, kvsProtocolAppendFn append, void *ud) {
// msg = "*2\r\n$3\r\nGET\r\n$3\r\nkey\r\n";
// length = 17;

    if (!msg || length <= 0 || !append) {
        g_kvs_last_consumed = 0;
        return 0;
    }

    char *p = msg;
    int remaining = length;
    while (remaining > 0 && (*p == '\r' || *p == '\n' || *p == ' ' || *p == '\t')) { // 跳过前导空白字符
        p++;
        remaining--;
    }
    if (remaining <= 0) {
        g_kvs_last_consumed = length;
        return 0;
    }

    if (*p != '*') { // 不是RESP格式
        char outbuf[BUFFER_LENGTH];
        int outlen = kvs_protocol_internal(msg, length, outbuf); // 走文本协议处理
        if (outlen <= 0) return 0;
        int n = append(outbuf, outlen, ud);
        if (n < 0) return n;
        if (n > outlen) n = outlen;
        return n;
    }

    /*支持大 value（博客） */
    int total_appended = 0;
    char *in = msg;
    remaining = length;

    while (remaining > 0) {
        while (remaining > 0 && (*in == '\r' || *in == '\n' || *in == ' ' || *in == '\t')) {
            in++;
            remaining--;
        }
        if (remaining <= 0) break;

        if (*in != '*') {
            static const char *err = "-ERR protocol error\r\n";
            int n = append(err, (int)strlen(err), ud);
            if (n < 0) return n;
            total_appended += n;
            remaining = 0;
            break;
        }

        char *tokens[KVS_MAX_TOKENS] = {0};
        int consumed = 0;
        int argc = kvs_parse_resp(in, remaining, tokens, &consumed); // 解析命令得到 argc 和 tokens
        if (argc < 0) {
            static const char *err = "-ERR protocol error\r\n";
            int n = append(err, (int)strlen(err), ud);
            if (n < 0) return n;
            total_appended += n;
            remaining = 0;
            break;
        }
        if (argc == 0) {
            /* 半包：等待更多数据 */
            break;
        }
        if (argc <= 0 || !tokens[0]) {
            static const char *err = "-ERR protocol error\r\n";
            int n = append(err, (int)strlen(err), ud);
            if (n < 0) return n;
            total_appended += n;
            in += consumed;
            remaining -= consumed;
            continue;
        }

        kvs_str_to_upper(tokens[0]);
        const char *cmd = tokens[0];

        if ((strcmp(cmd, "GET") == 0 || strcmp(cmd, "RGET") == 0 ||
             strcmp(cmd, "HGET") == 0 || strcmp(cmd, "LGET") == 0) && argc >= 2) {
            char *val = NULL;
            if (strcmp(cmd, "GET") == 0) {
                val = kvs_array_get(&global_array, tokens[1]);
            } else if (strcmp(cmd, "RGET") == 0) {
                val = kvs_rbtree_get(&global_rbtree, tokens[1]);
            } else if (strcmp(cmd, "HGET") == 0) {
                val = kvs_hash_get(&global_hash, tokens[1]);
            } else { // LGET
                val = kvs_skiplist_get(&global_skiplist, tokens[1]);
            }

            if (!val) {
                static const char *null_bulk = "$-1\r\n";
                int n = append(null_bulk, (int)strlen(null_bulk), ud);
                if (n < 0) return n;
                total_appended += n;
            } else {
                int vlen = (int)strlen(val);
                char header[64];
                int hlen = snprintf(header, sizeof(header), "$%d\r\n", vlen); // 首先发送长度头：$sizeof(header)\r\n
                if (hlen < 0) hlen = 0;
                int n = append(header, hlen, ud);
                if (n < 0) return n;
                total_appended += n;

                if (vlen > 0) {
                    n = append(val, vlen, ud);
                    if (n < 0) return n;
                    total_appended += n;
                }

                static const char *crlf = "\r\n";
                n = append(crlf, 2, ud);
                if (n < 0) return n;
                total_appended += n;
            }

        } else {
            int is_write_cmd = 0;
            const kvsCommand *command = kvs_lookup_command(cmd);
            if (!command || !command->proc) {
                char errbuf[128];
                int elen = snprintf(errbuf, sizeof(errbuf), "-ERR unknown command '%s'\r\n", cmd ? cmd : "");
                if (elen < 0) elen = 0;
                int n = append(errbuf, elen, ud);
                if (n < 0) return n;
                total_appended += n;
            } else {
                char inner[BUFFER_LENGTH];
                memset(inner, 0, sizeof(inner));
                int inner_len = command->proc(argc, tokens, inner, &is_write_cmd, 1 /*is_resp*/);
                if (inner_len < 0) inner_len = 0;

                if (kvs_is_raw_resp_reply(inner, inner_len)) {
                    int n = append(inner, inner_len, ud);
                    if (n < 0) return n;
                    total_appended += n;
                } else {
                    char outbuf[BUFFER_LENGTH];
                    int written = kvs_format_resp_bulk(inner, inner_len, outbuf, (int)sizeof(outbuf));
                    if (written < 0) written = 0;
                    int n = append(outbuf, written, ud);
                    if (n < 0) return n;
                    total_appended += n;
                }

#ifdef PERSISTENCE_ENABLE
                if (is_write_cmd) {
                    kvs_persistence_on_write_argv(argc, (const char * const *)tokens);

                    if (kvs_server.config.enable_persistence && kvs_server.config.enable_aof) {
                        (void)kvs_aof_append_argv(argc, (const char * const *)tokens);
                    }
                }
#endif

            }
        }

        in += consumed;
        remaining -= consumed;
    }

    g_kvs_last_consumed = length - remaining;
    if (g_kvs_last_consumed < 0) g_kvs_last_consumed = 0;
    return total_appended;
}

int kvs_protocol(char *msg, int length, char *response_buf) {
	return kvs_protocol_internal(msg, length, response_buf);
}

int kvs_protocol_last_consumed(void) {
	return g_kvs_last_consumed;
}