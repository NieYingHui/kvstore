#include <stddef.h>
#include <string.h>

#include "client.h"
#include "protocol.h"
#include "engine.h"

/* 客户端缓冲区初始大小，与协议层 BUFFER_LENGTH 保持一致（1024） */
#define KVS_CLIENT_INIT_BUF 1024

// 确保KVS（键值存储）客户端查询缓冲区容量
// 0: 成功或无需调整
// -1: 内存分配失败
static int kvs_client_ensure_query_capacity(kvsClient *client, int add_len) {
	if (!client || add_len <= 0) return 0;
	int needed = client->querylen + add_len;
	if (needed <= client->querycap) return 0;
	int newcap = client->querycap ? client->querycap : KVS_CLIENT_INIT_BUF;
	while (newcap < needed) {
		newcap *= 2;
	}
	char *p = (char *)kvs_malloc((size_t)newcap);
	if (!p) return -1;
	if (client->querybuf && client->querylen > 0) {
		memcpy(p, client->querybuf, (size_t)client->querylen);
	}
	if (client->querybuf) {
		kvs_free(client->querybuf);
	}
	client->querybuf = p;
	client->querycap = newcap;
	return 0;
}

// 确保客户端的回复缓冲区有足够的容量来存储新的数据
// 0: 成功或无需调整
// -1: 内存分配失败
static int kvs_client_ensure_reply_capacity(kvsClient *client, int add_len) {
	if (!client || add_len <= 0) return 0;
	int needed = client->replylen + add_len;
	if (needed <= client->replycap) return 0;
	int newcap = client->replycap ? client->replycap : KVS_CLIENT_INIT_BUF;
	while (newcap < needed) {
		newcap *= 2;
	}
	char *p = (char *)kvs_malloc((size_t)newcap);
	if (!p) return -1;
	if (client->replybuf && client->replylen > 0) {
		memcpy(p, client->replybuf, (size_t)client->replylen);
	}
	if (client->replybuf) {
		kvs_free(client->replybuf);
	}
	client->replybuf = p;
	client->replycap = newcap;
	return 0;
}

/* 协议层流式回调：将响应追加到 kvsClient.replybuf 中。 */ 
// typedef int (*kvsProtocolAppendFn)(const char *data, int len, void *ud);
static int kvs_client_reply_append(const char *data, int len, void *ud) {
	kvsClient *c = (kvsClient *)ud;
	if (!c || !data || len <= 0) return 0;
	if (kvs_client_ensure_reply_capacity(c, len) != 0) return -1;
	memcpy(c->replybuf + c->replylen, data, (size_t)len);
	c->replylen += len;
	return len;
}

/* 追加原始响应字节到 replybuf */
int kvs_client_append_reply(kvsClient *client, const char *data, int len) {
	if (!client || !data || len <= 0) return 0;
	if (kvs_client_ensure_reply_capacity(client, len) != 0) return -1;
	memcpy(client->replybuf + client->replylen, data, (size_t)len);
	client->replylen += len;
	return len;
}

kvsClient *kvs_client_create(int fd) {
	kvsClient *c = (kvsClient *)kvs_malloc(sizeof(kvsClient));
	if (!c) return NULL;
	memset(c, 0, sizeof(kvsClient));
	c->fd = fd;
	return c;
}

void kvs_client_free(kvsClient *client) {
	if (!client) return;
	if (client->querybuf) kvs_free(client->querybuf);
	if (client->replybuf) kvs_free(client->replybuf);
	kvs_free(client);
}

/* 将新收到的数据追加到客户端输入缓冲区 */
int kvs_client_append_query(kvsClient *client, const char *data, int len) {
	if (!client || !data || len <= 0) return 0;
	if (kvs_client_ensure_query_capacity(client, len) != 0) return -1;
	memcpy(client->querybuf + client->querylen, data, (size_t)len);
	client->querylen += len;
	return len;
}

/*
 * 处理 querybuf 中尽可能多的完整命令，
 * 生成的回复追加到 replybuf，返回本次新增回复的字节数。
 */
int kvs_process_client(kvsClient *client) {
	if (!client || client->querylen <= 0) return 0;
	int total_new_reply = 0;

	while (client->querylen > 0) {
		/* 使用流式协议接口，将输出直接追加到 replybuf。 */
		int n = kvs_protocol_stream(
			client->querybuf,
			client->querylen,
			kvs_client_reply_append,
			client);
		int consumed = kvs_protocol_last_consumed();
		if (consumed <= 0) {
			/* 没有消费任何数据，说明前缀命令不完整，等待更多数据 */
			break;
		}
		if (n > 0) {
			total_new_reply += n;
		}
		/* 将未处理的数据前移，保留半包部分 */
		if (consumed < client->querylen) {
			memmove(client->querybuf,
				client->querybuf + consumed,
				(size_t)(client->querylen - consumed));
		}
		client->querylen -= consumed;
	}

	return total_new_reply;
}
