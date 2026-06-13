#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../hiredis/hiredis.h"

static void die_redis(const char *msg, redisContext *c) { // 接收一个Redis连接上下文指针
    if (c && c->err) {
        fprintf(stderr, "%s: %s\n", msg, c->errstr);
    } else {
        fprintf(stderr, "%s\n", msg);
    }
    exit(1);
}

static void expect_reply(redisReply *r) { // 检查Redis命令执行后的回复是否为空
    if (!r) {
        fprintf(stderr, "NULL reply (connection dropped?)\n");
        exit(1);
    }
}

int main(int argc, char **argv) {
    const char *host = "127.0.0.1";
    int port = 2000;
    if (argc >= 2) host = argv[1];
    if (argc >= 3) port = atoi(argv[2]);

    struct timeval timeout = {1, 500000}; /* 1.5s */
    redisContext *c = redisConnectWithTimeout(host, port, timeout); // 同步链接
    if (!c) die_redis("redisConnectWithTimeout failed", c);
    if (c->err) die_redis("redis connection error", c);

    redisReply *r = NULL;

    r = (redisReply *)redisCommand(c, "PING"); // 同步命令执行函数
    expect_reply(r);
    if (r->type != REDIS_REPLY_STATUS) {
        fprintf(stderr, "PING unexpected type=%d\n", r->type);
        freeReplyObject(r);
        return 2;
    }
    printf("PING => %s\n", r->str);
    freeReplyObject(r);

    r = (redisReply *)redisCommand(c, "SET %s %s", "k1", "v1");
    expect_reply(r);
    if (r->type != REDIS_REPLY_STATUS || strcmp(r->str, "OK") != 0) {
        fprintf(stderr, "SET unexpected reply\n");
        freeReplyObject(r);
        return 2;
    }
    printf("SET k1 v1 => %s\n", r->str);
    freeReplyObject(r);

    r = (redisReply *)redisCommand(c, "GET %s", "k1");
    expect_reply(r);
    if (r->type != REDIS_REPLY_STRING) {
        fprintf(stderr, "GET unexpected type=%d\n", r->type);
        freeReplyObject(r);
        return 2;
    }
    printf("GET k1 => %s\n", r->str);
    freeReplyObject(r);

    r = (redisReply *)redisCommand(c, "EXISTS %s", "k1");
    expect_reply(r);
    if (r->type != REDIS_REPLY_INTEGER) {
        fprintf(stderr, "EXISTS unexpected type=%d\n", r->type);
        freeReplyObject(r);
        return 2;
    }
    printf("EXISTS k1 => %lld\n", (long long)r->integer);
    freeReplyObject(r);

    r = (redisReply *)redisCommand(c, "DEL %s", "k1");
    expect_reply(r);
    if (r->type != REDIS_REPLY_INTEGER) {
        fprintf(stderr, "DEL unexpected type=%d\n", r->type);
        freeReplyObject(r);
        return 2;
    }
    printf("DEL k1 => %lld\n", (long long)r->integer);
    freeReplyObject(r);

    /* Hash engine (kvstore uses a separate hash keyspace, not Redis hash-of-hash). */
    r = (redisReply *)redisCommand(c, "HSET %s %s", "hk1", "hv1");
    expect_reply(r);
    if (r->type != REDIS_REPLY_INTEGER) {
        fprintf(stderr, "HSET unexpected type=%d\n", r->type);
        freeReplyObject(r);
        return 2;
    }
    printf("HSET hk1 hv1 => %lld\n", (long long)r->integer);
    freeReplyObject(r);

    r = (redisReply *)redisCommand(c, "HGET %s", "hk1");
    expect_reply(r);
    if (r->type != REDIS_REPLY_STRING) {
        fprintf(stderr, "HGET unexpected type=%d\n", r->type);
        freeReplyObject(r);
        return 2;
    }
    printf("HGET hk1 => %s\n", r->str);
    freeReplyObject(r);

    /* RBTree engine (custom command names). */
    r = (redisReply *)redisCommand(c, "RSET %s %s", "rk1", "rv1");
    expect_reply(r);
    if (r->type != REDIS_REPLY_STATUS || strcmp(r->str, "OK") != 0) {
        fprintf(stderr, "RSET unexpected reply\n");
        freeReplyObject(r);
        return 2;
    }
    printf("RSET rk1 rv1 => %s\n", r->str);
    freeReplyObject(r);

    r = (redisReply *)redisCommand(c, "RGET %s", "rk1");
    expect_reply(r);
    if (r->type != REDIS_REPLY_STRING) {
        fprintf(stderr, "RGET unexpected type=%d\n", r->type);
        freeReplyObject(r);
        return 2;
    }
    printf("RGET rk1 => %s\n", r->str);
    freeReplyObject(r);

    /* 跳表引擎（自定义命令名称）。 */
    r = (redisReply *)redisCommand(c, "LSET %s %s", "lk1", "lv1");
    expect_reply(r);
    if (r->type != REDIS_REPLY_STATUS || strcmp(r->str, "OK") != 0) {
        fprintf(stderr, "LSET unexpected reply\n");
        freeReplyObject(r);
        return 2;
    }
    printf("LSET lk1 lv1 => %s\n", r->str);
    freeReplyObject(r);

    r = (redisReply *)redisCommand(c, "LGET %s", "lk1");
    expect_reply(r);
    if (r->type != REDIS_REPLY_STRING) {
        fprintf(stderr, "LGET unexpected type=%d\n", r->type);
        freeReplyObject(r);
        return 2;
    }
    printf("LGET lk1 => %s\n", r->str);
    freeReplyObject(r);

    redisFree(c);
    return 0;
}
