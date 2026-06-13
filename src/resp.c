#include "resp.h"

#include <string.h>
#include <stdio.h>

static int kvs_resp_append_all(kvsProtocolAppendFn append, void *ud,
                              const char *data, int len) { // 数据追加
    if (!append || !data || len <= 0) return 0;
    int n = append(data, len, ud);
    if (n < 0) return n;
    return n;
}

// 批量数据（bulk string）的响应格式化data = "hello" -> ud : "$5\r\nhello\r\n"
int kvs_resp_append_bulk_len(kvsProtocolAppendFn append, void *ud,
                            const char *data, int len) { 
    if (!append) return -1;

    if (!data) {
        static const char *null_bulk = "$-1\r\n";
        return kvs_resp_append_all(append, ud, null_bulk, (int)strlen(null_bulk));
    }
    if (len < 0) len = 0;

    char header[64];
    int hlen = snprintf(header, sizeof(header), "$%d\r\n", len); // $5\r\n
    if (hlen < 0) hlen = 0;

    int n = kvs_resp_append_all(append, ud, header, hlen);
    if (n < 0) return n;

    if (len > 0) {
        n = kvs_resp_append_all(append, ud, data, len);
        if (n < 0) return n;
    }

    static const char *crlf = "\r\n";
    n = kvs_resp_append_all(append, ud, crlf, 2);
    if (n < 0) return n;

    return hlen + len + 2;
}

// 将一个字符串数组以RESP（Redis Serialization Protocol）格式追加到输出缓冲区
// *argv[] = {"SET", "key", "value"}; -> *3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n
int kvs_resp_append_array_argv(kvsProtocolAppendFn append, void *ud,
                              int argc, const char * const *argv) { 
    if (!append || argc <= 0 || !argv) return -1;

    char header[64];
    int hlen = snprintf(header, sizeof(header), "*%d\r\n", argc); // *3\r\n
    if (hlen < 0) hlen = 0;

    int total = 0;
    int n = kvs_resp_append_all(append, ud, header, hlen); // $3\r\n
    if (n < 0) return n;
    total += hlen;

    for (int i = 0; i < argc; i++) {
        const char *s = argv[i] ? argv[i] : "";
        int slen = (int)strlen(s);
        n = kvs_resp_append_bulk_len(append, ud, s, slen);
        if (n < 0) return n;
        total += n;
    }

    return total;
}
