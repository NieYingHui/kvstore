
#include "kvs_rdb.h"

#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#include "../../kvstore.h"
#include "../engine.h"

/*
 * RDB 持久化（对齐 Redis 的文件格式）：
 * - 写入：生成标准的 RDB v9（"REDIS0009"），并带 CRC64 校验。
 * - 读取：支持加载 Redis 生成的 RDB（仅支持 string key/value 最小子集）。
 *
 * 由于本项目内部存在 4 套 KV 存储结构，为了在单个 RDB 文件中保存它们：
 *   DB0=Array, DB1=Hash, DB2=RBTree, DB3=SkipList
 */

/* --- Redis RDB 常量（最小子集） --- */
#define RDB_VERSION_STR "0009"
#define RDB_OPCODE_AUX           0xFA // 辅助字段操作码，用于存储元数据
#define RDB_OPCODE_RESIZEDB      0xFB // 调整哈希表大小操作码
#define RDB_OPCODE_EXPIRETIME_MS 0xFC // 设置毫秒级过期时间操作码
#define RDB_OPCODE_EXPIRETIME    0xFD // 设置秒级过期时间操作码
#define RDB_OPCODE_SELECTDB      0xFE // 选择数据库操作码
#define RDB_OPCODE_EOF           0xFF // 文件结束操作码

#define RDB_TYPE_STRING 0 // 字符串类型标识符

/* --- CRC64 (ECMA-182), Redis checksum --- */
static uint64_t crc64_tab[256];
static int crc64_tab_inited = 0; // 标志位，表示查找表是否已初始化

// 这种实现方式通过空间换时间，使用256个64位整数的内存空间来换取计算速度的提升
static void crc64_init(void) {
    if (crc64_tab_inited) return;
    const uint64_t poly = 0x42F0E1EBA9EA3693ULL; // （ECMA-182标准）
    for (int i = 0; i < 256; ++i) {
        uint64_t crc = (uint64_t)i << 56;
        for (int j = 0; j < 8; ++j) {
            crc = (crc & 0x8000000000000000ULL) ? ((crc << 1) ^ poly) : (crc << 1);
        }
        crc64_tab[i] = crc;
    }
    crc64_tab_inited = 1;
}

// 计算和更新数据的CRC64校验值
static uint64_t crc64_update(uint64_t crc, const unsigned char *s, size_t l) {
    crc64_init();
    while (l--) {
        crc = crc64_tab[((crc >> 56) ^ *s++) & 0xFF] ^ (crc << 8);
    }
    return crc;
}

static int ensure_log_dir_exists(void) {
    const char *log_dir = (kvs_server.config.dir[0] != '\0')
                            ? kvs_server.config.dir
                            : "log";
    struct stat st = {0};
    if (stat(log_dir, &st) == -1) {
        if (mkdir(log_dir, 0755) != 0) return -1;
    }
    return 0;
}

// 将数据写入文件并更新CRC校验值
static int fwrite_crc(FILE *fp, const void *buf, size_t n, uint64_t *crc) {
    if (!fp || !buf || n == 0) return 0;
    if (fwrite(buf, 1, n, fp) != n) return -1;
    if (crc) *crc = crc64_update(*crc, (const unsigned char *)buf, n);
    return 0;
}

// 向文件中写入一个8位无符号整数(uint8_t)
static int fwrite_u8(FILE *fp, uint8_t v, uint64_t *crc) {
    return fwrite_crc(fp, &v, 1, crc);
}

// 将64位无符号整数以小端序格式写入文件
static int fwrite_u64_le(FILE *fp, uint64_t v, uint64_t *crc) {
    unsigned char b[8];
    for (int i = 0; i < 8; ++i) b[i] = (unsigned char)((v >> (8 * i)) & 0xFF);
    return fwrite_crc(fp, b, 8, crc);
}

/* RDB长度编码：仅支持常规长度（6/14/32位）。 */
// 将长度值以紧凑格式写入文件的函数
static int rdb_write_len(FILE *fp, uint32_t len, uint64_t *crc) {
    if (len < (1U << 6)) { // 小长度值编码（0-63）
        uint8_t b = (uint8_t)(len & 0x3F); /* 00xxxxxx */
        return fwrite_u8(fp, b, crc);
    }
    if (len < (1U << 14)) { // 中等长度值编码（64-16383）
        uint8_t b0 = (uint8_t)(0x40 | ((len >> 8) & 0x3F)); /* 01xxxxxx */ // 第一个字节格式：01xxxxxx（前两位为01，后6位存储len的高8位）
        uint8_t b1 = (uint8_t)(len & 0xFF); // 第二个字节存储len的低8位
        if (fwrite_u8(fp, b0, crc) != 0) return -1;
        return fwrite_u8(fp, b1, crc); 
    }

    /* 10...... + 32位大端 */ // 大长度值编码（≥16384）
    if (fwrite_u8(fp, 0x80, crc) != 0) return -1; 
    unsigned char be[4]; // 接下来的4个字节以大端序存储32位长度值
    be[0] = (unsigned char)((len >> 24) & 0xFF);
    be[1] = (unsigned char)((len >> 16) & 0xFF);
    be[2] = (unsigned char)((len >> 8) & 0xFF);
    be[3] = (unsigned char)(len & 0xFF);
    return fwrite_crc(fp, be, 4, crc);
}

// 将字符串写入RDB文件的函数
static int rdb_write_string(FILE *fp, const char *s, uint64_t *crc) {
    if (!s) s = "";
    size_t len = strlen(s);
    if (len > 0xFFFFFFFFu) return -1; // 长度是否超过32位无符号整数的最大值
    if (rdb_write_len(fp, (uint32_t)len, crc) != 0) return -1;
    return (len == 0) ? 0 : fwrite_crc(fp, s, len, crc);
}

// 在RDB文件中写入SELECTDB操作码和数据库ID
static int rdb_write_selectdb(FILE *fp, uint32_t dbid, uint64_t *crc) {
    if (fwrite_u8(fp, RDB_OPCODE_SELECTDB, crc) != 0) return -1;
    return rdb_write_len(fp, dbid, crc);
}

// 用于在RDB文件中写入数据库大小和过期键的数量信息
static int rdb_write_resizedb(FILE *fp, uint32_t dbsize, uint32_t expires, uint64_t *crc) {
    if (fwrite_u8(fp, RDB_OPCODE_RESIZEDB, crc) != 0) return -1;
    if (rdb_write_len(fp, dbsize, crc) != 0) return -1;
    return rdb_write_len(fp, expires, crc);
}

// 写入键值对字符串的函数
static int rdb_write_kv_string(FILE *fp, const char *k, const char *v, uint64_t *crc) {
    if (fwrite_u8(fp, RDB_TYPE_STRING, crc) != 0) return -1;
    if (rdb_write_string(fp, k, crc) != 0) return -1;
    return rdb_write_string(fp, v, crc);
}

#if ENABLE_RBTREE
static uint32_t rbtree_count(kvs_rbtree_t *t, rbtree_node *node) {
    if (!t || !node || node == t->nil) return 0;
    uint32_t n = 0;
    n += rbtree_count(t, node->left);
    if (node->key && node->value) n += 1;
    n += rbtree_count(t, node->right);
    return n;
}

static int rbtree_write(FILE *fp, kvs_rbtree_t *t, rbtree_node *node, uint64_t *crc) {
    if (!t || !node || node == t->nil) return 0;
    if (rbtree_write(fp, t, node->left, crc) != 0) return -1;
    if (node->key && node->value) {
        if (rdb_write_kv_string(fp, node->key, (const char *)node->value, crc) != 0) return -1;
    }
    return rbtree_write(fp, t, node->right, crc);
}
#endif

// 将内存中的多种数据结构（数组、哈希表、红黑树、跳表）持久化到磁盘文件中，采用RDB格式
int kvs_rdb_save_to_file(const char *path) {
    if (!path || path[0] == '\0') return -1;
    (void)ensure_log_dir_exists();

    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;

    uint64_t crc = 0;
    if (fwrite_crc(fp, "REDIS", 5, &crc) != 0) { fclose(fp); return -1; }
    if (fwrite_crc(fp, RDB_VERSION_STR, 4, &crc) != 0) { fclose(fp); return -1; }

    /* DB0: Array */
    uint32_t dbsize = 0;
#if ENABLE_ARRAY
    if (global_array.table) {
        for (int i = 0; i < global_array.total; ++i) {
            if (global_array.table[i].key && global_array.table[i].value) dbsize++;
        }
    }
#endif
    if (rdb_write_selectdb(fp, 0, &crc) != 0) { fclose(fp); return -1; }
    if (rdb_write_resizedb(fp, dbsize, 0, &crc) != 0) { fclose(fp); return -1; }
#if ENABLE_ARRAY
    if (global_array.table) {
        for (int i = 0; i < global_array.total; ++i) {
            const char *k = global_array.table[i].key;
            const char *v = global_array.table[i].value;
            if (!k || !v) continue;
            if (rdb_write_kv_string(fp, k, v, &crc) != 0) { fclose(fp); return -1; }
        }
    }
#endif

    /* DB1: Hash */
    dbsize = 0;
#if ENABLE_HASH
    if (global_hash.nodes) {
        for (int i = 0; i < global_hash.max_slots; ++i) {
            for (hashnode_t *node = global_hash.nodes[i]; node; node = node->next) {
                if (node->key && node->value) dbsize++;
            }
        }
    }
#endif
    if (rdb_write_selectdb(fp, 1, &crc) != 0) { fclose(fp); return -1; }
    if (rdb_write_resizedb(fp, dbsize, 0, &crc) != 0) { fclose(fp); return -1; }
#if ENABLE_HASH
    if (global_hash.nodes) {
        for (int i = 0; i < global_hash.max_slots; ++i) {
            for (hashnode_t *node = global_hash.nodes[i]; node; node = node->next) {
                if (!node->key || !node->value) continue;
                if (rdb_write_kv_string(fp, node->key, node->value, &crc) != 0) { fclose(fp); return -1; }
            }
        }
    }
#endif

    /* DB2: RBTree */
    dbsize = 0;
#if ENABLE_RBTREE
    if (global_rbtree.root && global_rbtree.nil) {
        dbsize = rbtree_count(&global_rbtree, global_rbtree.root);
    }
#endif
    if (rdb_write_selectdb(fp, 2, &crc) != 0) { fclose(fp); return -1; }
    if (rdb_write_resizedb(fp, dbsize, 0, &crc) != 0) { fclose(fp); return -1; }
#if ENABLE_RBTREE
    if (global_rbtree.root && global_rbtree.nil) {
        if (rbtree_write(fp, &global_rbtree, global_rbtree.root, &crc) != 0) { fclose(fp); return -1; }
    }
#endif

    /* DB3: SkipList */
    dbsize = 0;
#if ENABLE_SKIPLIST
    if (global_skiplist.header) {
        Node *node = global_skiplist.header->forward[0];
        while (node && node->key) {
            if (node->key && node->value) dbsize++;
            node = node->forward[0];
        }
    }
#endif
    if (rdb_write_selectdb(fp, 3, &crc) != 0) { fclose(fp); return -1; }
    if (rdb_write_resizedb(fp, dbsize, 0, &crc) != 0) { fclose(fp); return -1; }
#if ENABLE_SKIPLIST
    if (global_skiplist.header) {
        Node *node = global_skiplist.header->forward[0];
        while (node && node->key) {
            if (node->key && node->value) {
                if (rdb_write_kv_string(fp, node->key, node->value, &crc) != 0) { fclose(fp); return -1; }
            }
            node = node->forward[0];
        }
    }
#endif

    if (fwrite_u8(fp, RDB_OPCODE_EOF, &crc) != 0) { fclose(fp); return -1; } // 写入结束标记(RDB_OPCODE_EOF)
    if (fwrite_u64_le(fp, crc, NULL) != 0) { fclose(fp); return -1; } // 写入CRC校验值

    fflush(fp);
    fclose(fp);
    return 0;
}


/* --- 读取器（最小子集） --- */ // 从内存中读取一个8位无符号整数（uint8_t）
static int rdb_read_u8(const unsigned char *p, const unsigned char *end, uint8_t *out) {
    if (!p || !out || p >= end) return -1;
    *out = *p;
    return 0;
}

// 从字节流中读取32位大端序（big-endian）无符号整数的函数
static int rdb_read_u32_be(const unsigned char *p, const unsigned char *end, uint32_t *out) {
    if (!p || !out || (end - p) < 4) return -1;
    *out = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    return 0;
}

// 从字节流中读取32位小端序无符号整数的函数
static int rdb_read_u32_le(const unsigned char *p, const unsigned char *end, uint32_t *out) {
    if (!p || !out || (end - p) < 4) return -1;
    *out = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return 0;
}

// 从字节流中读取64位小端序无符号整数的函数
static int rdb_read_u64_le(const unsigned char *p, const unsigned char *end, uint64_t *out) {
    if (!p || !out || (end - p) < 8) return -1;
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | (uint64_t)p[i];
    *out = v;
    return 0;
}

/* 返回 0=需要更多/错误，1=成功 */
static int rdb_read_len(const unsigned char **pp, const unsigned char *end, uint32_t *len_out, int *is_encoded_out, uint8_t *enc_type_out) {
    const unsigned char *p = *pp;
    if (p >= end) return 0;
    uint8_t b0 = *p++;
    uint8_t type = (b0 >> 6) & 0x03; // 确定编码类型

    if (type == 0) { // 6位长度编码 表示长度（0-63）
        *len_out = (uint32_t)(b0 & 0x3F);
        if (is_encoded_out) *is_encoded_out = 0;
        *pp = p;
        return 1;
    }
    if (type == 1) { // 14位长度编码 表示长度（0-16383）
        if (p >= end) return 0;
        uint8_t b1 = *p++;
        *len_out = (uint32_t)(((uint32_t)(b0 & 0x3F) << 8) | b1);
        if (is_encoded_out) *is_encoded_out = 0;
        *pp = p;
        return 1;
    }
    if (type == 2) { // 32位长度编码 读取接下来的4个大端字节表示32位长度
        if ((end - p) < 4) return 0;
        uint32_t v = 0;
        if (rdb_read_u32_be(p, end, &v) != 0) return 0;
        p += 4;
        *len_out = v;
        if (is_encoded_out) *is_encoded_out = 0;
        *pp = p;
        return 1;
    }

    /* 类型 == 3：特殊编码 */ // 低6位（b0 & 0x3F）表示编码类型
    if (is_encoded_out) *is_encoded_out = 1;
    if (enc_type_out) *enc_type_out = (uint8_t)(b0 & 0x3F);
    *pp = p;
    return 1;
}

/* 最小化 LZF 解压缩器（仅用于读取 RDB）。 */
static int lzf_decompress(const unsigned char *in, unsigned int in_len, unsigned char *out, unsigned int out_len) {
    unsigned int iidx = 0;
    unsigned int oidx = 0;
    while (iidx < in_len) {
        unsigned char ctrl = in[iidx++];
        if (ctrl < (1 << 5)) { // 字面量
            unsigned int lit = (unsigned int)ctrl + 1;
            if (iidx + lit > in_len) return -1;
            if (oidx + lit > out_len) return -1;
            memcpy(out + oidx, in + iidx, lit);
            iidx += lit;
            oidx += lit;
        } else { // 引用
            unsigned int len = (unsigned int)(ctrl >> 5);
            unsigned int ref = (unsigned int)(ctrl & 0x1F) << 8;
            if (iidx >= in_len) return -1;
            ref |= in[iidx++];
            if (len == 7) {
                if (iidx >= in_len) return -1;
                len += in[iidx++];
            }
            ref += 1;
            len += 2;
            if (ref > oidx) return -1;
            if (oidx + len > out_len) return -1;
            for (unsigned int k = 0; k < len; ++k) {
                out[oidx + k] = out[oidx - ref + k];
            }
            oidx += len;
        }
    }
    return (int)oidx;
}

// 读取RDB格式中字符串数据
static int rdb_read_string(const unsigned char **pp, const unsigned char *end, char **out_str) {
    uint32_t len = 0;
    int is_encoded = 0;
    uint8_t enc_type = 0;
    int r = rdb_read_len(pp, end, &len, &is_encoded, &enc_type); // 获取长度len
    if (r == 0) return -1;

    if (!is_encoded) {
        if ((uint32_t)(end - *pp) < len) return -1;
        char *s = (char *)kvs_malloc((size_t)len + 1);
        if (!s) return -1;
        if (len > 0) memcpy(s, *pp, (size_t)len);
        s[len] = '\0';
        *pp += len;
        *out_str = s;
        return 0;
    }

    /* 编码字符串：整数或LZF */
    if (enc_type == 0) { // 编码类型0：读取8位整数并转为字符串
        if (*pp >= end) return -1;
        int8_t v = (int8_t)(*(*pp)++);
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "%d", (int)v);
        *out_str = (char *)kvs_malloc(strlen(tmp) + 1);
        if (!*out_str) return -1;
        strcpy(*out_str, tmp);
        return 0;
    }
    if (enc_type == 1) { // 编码类型1：读取16位整数并转为字符串
        if ((end - *pp) < 2) return -1;
        uint16_t vv = (uint16_t)((*pp)[0] | ((*pp)[1] << 8));
        *pp += 2;
        int16_t v = (int16_t)vv;
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "%d", (int)v);
        *out_str = (char *)kvs_malloc(strlen(tmp) + 1);
        if (!*out_str) return -1;
        strcpy(*out_str, tmp);
        return 0;
    }
    if (enc_type == 2) { // 编码类型2：读取32位整数并转为字符串
        if ((end - *pp) < 4) return -1;
        uint32_t vv = 0;
        if (rdb_read_u32_le(*pp, end, &vv) != 0) return -1;
        *pp += 4;
        int32_t v = (int32_t)vv;
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "%d", (int)v);
        *out_str = (char *)kvs_malloc(strlen(tmp) + 1);
        if (!*out_str) return -1;
        strcpy(*out_str, tmp);
        return 0;
    }
    if (enc_type == 3) { // 编码类型3：处理LZF压缩数据
        uint32_t clen = 0, ulen = 0;
        int ie = 0;
        uint8_t et = 0;
        if (rdb_read_len(pp, end, &clen, &ie, &et) == 0 || ie) return -1;
        if (rdb_read_len(pp, end, &ulen, &ie, &et) == 0 || ie) return -1;
        if ((uint32_t)(end - *pp) < clen) return -1;
        unsigned char *out = (unsigned char *)kvs_malloc((size_t)ulen + 1);
        if (!out) return -1;
        int n = lzf_decompress(*pp, clen, out, ulen);
        *pp += clen;
        if (n < 0 || (uint32_t)n != ulen) {
            kvs_free(out);
            return -1;
        }
        out[ulen] = '\0';
        *out_str = (char *)out;
        return 0;
    }

    return -1;
}

// 用于加载RDB文件的函数
static int kvs_rdb_load_redis(const unsigned char *p, size_t size) {
    if (!p || size < 9) return -1;
    if (memcmp(p, "REDIS", 5) != 0) return -1;

    const unsigned char *cur = p + 9;
    const unsigned char *end = p + size;
    uint32_t current_db = 0;
    int pending_expire = 0;
    (void)pending_expire;

    while (cur < end) {
        uint8_t op = 0;
        if (rdb_read_u8(cur, end, &op) != 0) return -1;
        cur++;

        if (op == RDB_OPCODE_EOF) {
            break;
        }
        if (op == RDB_OPCODE_AUX) {
            char *k = NULL, *v = NULL;
            if (rdb_read_string(&cur, end, &k) != 0) return -1;
            if (rdb_read_string(&cur, end, &v) != 0) { kvs_free(k); return -1; }
            kvs_free(k);
            kvs_free(v);
            continue;
        }
        if (op == RDB_OPCODE_SELECTDB) {
            uint32_t dbid = 0;
            int ie = 0;
            uint8_t et = 0;
            if (rdb_read_len(&cur, end, &dbid, &ie, &et) == 0 || ie) return -1;
            current_db = dbid;
            continue;
        }
        if (op == RDB_OPCODE_RESIZEDB) {
            uint32_t a = 0, b = 0;
            int ie = 0;
            uint8_t et = 0;
            if (rdb_read_len(&cur, end, &a, &ie, &et) == 0 || ie) return -1;
            if (rdb_read_len(&cur, end, &b, &ie, &et) == 0 || ie) return -1;
            continue;
        }
        if (op == RDB_OPCODE_EXPIRETIME) {
            if ((end - cur) < 4) return -1;
            uint32_t sec = 0;
            if (rdb_read_u32_le(cur, end, &sec) != 0) return -1;
            cur += 4;
            (void)sec;
            pending_expire = 1;
            continue;
        }
        if (op == RDB_OPCODE_EXPIRETIME_MS) {
            if ((end - cur) < 8) return -1;
            uint64_t ms = 0;
            if (rdb_read_u64_le(cur, end, &ms) != 0) return -1;
            cur += 8;
            (void)ms;
            pending_expire = 1;
            continue;
        }

        /* 值类型 */
        if (op != RDB_TYPE_STRING) return -1;

        char *key = NULL;
        char *val = NULL;
        if (rdb_read_string(&cur, end, &key) != 0) return -1;
        if (rdb_read_string(&cur, end, &val) != 0) { kvs_free(key); return -1; }

        /* kvstore 没有 TTL 语义：忽略 EXPIRETIME(_MS) */
        pending_expire = 0;

        if (current_db == 0) kvs_array_set(&global_array, key, val);
        else if (current_db == 1) kvs_hash_set(&global_hash, key, val);
        else if (current_db == 2) kvs_rbtree_set(&global_rbtree, key, val);
        else if (current_db == 3) kvs_skiplist_set(&global_skiplist, key, val);
        else kvs_array_set(&global_array, key, val);

        kvs_free(key);
        kvs_free(val);
    }

    /* 校验和：若文件末尾后剩余字节数恰为8，则视为校验和 */
    if ((size_t)(end - cur) == 8) {
        uint64_t expected = 0;
        if (rdb_read_u64_le(cur, end, &expected) != 0) return -1;
        uint64_t actual = 0;
        actual = crc64_update(actual, p, (size_t)(cur - p));
        if (actual != expected) return -1;
    } else if (cur != end) {
        /* 冗余字节：不支持 */
        return -1;
    }

    return 0;
}

// 从文件加载数据的函数，使用mmap，调用kvs_rdb_load_redis
int kvs_rdb_load_from_file(const char *path) {
    if (!path || path[0] == '\0') return -1;

    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return -1;
    }

    void *data = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return -1;
    }

    int rc = -1;
    const unsigned char *p = (const unsigned char *)data;
    if ((size_t)st.st_size >= 9 && memcmp(p, "REDIS", 5) == 0) {
        rc = kvs_rdb_load_redis(p, (size_t)st.st_size);
    } else {
        rc = -1;
    }

    munmap(data, (size_t)st.st_size);
    close(fd);
    return rc;
}

int kvs_rdb_save(void) {
    const char *path = (kvs_server.config.rdb_path[0] != '\0')
                        ? kvs_server.config.rdb_path
                        : "log/dump.rdb";
    return kvs_rdb_save_to_file(path);
}

int kvs_rdb_load(void) {
    const char *path = (kvs_server.config.rdb_path[0] != '\0')
                        ? kvs_server.config.rdb_path
                        : "log/dump.rdb";
    return kvs_rdb_load_from_file(path);
}
