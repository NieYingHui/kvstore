#ifndef KVS_CLIENT_H
#define KVS_CLIENT_H

/*
 * 客户端抽象：
 * 封装单个连接的请求/响应缓冲区，
 * 让网络层只关心收发字节，协议和命令处理通过 handler 完成。
 */

typedef struct kvsClient {
	int fd;
	char *querybuf;   /* 输入缓冲区 */
	int  querylen;    /* 当前已使用字节数 */
	int  querycap;    /* 缓冲区总容量 */
	char *replybuf;   /* 输出缓冲区 */
	int  replylen;    /* 当前输出总字节数 */
	int  replycap;    /* 输出缓冲区总容量 */
	int  replypos;    /* 已发送的字节数，用于部分发送 */
} kvsClient;

typedef int (*kvsClientHandler)(kvsClient *client);

/* 客户端生命周期与流式处理接口 */
kvsClient *kvs_client_create(int fd);
void kvs_client_free(kvsClient *client);

/* 追加原始响应字节到 replybuf */
int kvs_client_append_reply(kvsClient *client, const char *data, int len);

/* 将新收到的数据追加到客户端输入缓冲区 */
int kvs_client_append_query(kvsClient *client, const char *data, int len);

/*
 * 处理 querybuf 中尽可能多的完整命令，
 * 生成的回复追加到 replybuf，返回本次新增回复的字节数。
 */
int kvs_process_client(kvsClient *client);

#endif /* KVS_CLIENT_H */
