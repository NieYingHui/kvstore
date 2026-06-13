



#ifndef __SERVER_H__
#define __SERVER_H__

#define BUFFER_LENGTH		1024


#define ENABLE_HTTP			0
#define ENABLE_WEBSOCKET	0
#define ENABLE_KVSTORE		1

typedef int (*RCALLBACK)(int fd);

/* 前向声明，避免头文件循环依赖 */
struct kvsClient;

struct conn {
	int fd;

	char rbuffer[BUFFER_LENGTH];
	int rlength;

	char wbuffer[BUFFER_LENGTH];
	int wlength;

	RCALLBACK send_callback;

	union {
		RCALLBACK recv_callback;
		RCALLBACK accept_callback;
	} r_action;

	int status;
#if 1 // websocket
	char *payload;
	char mask[4];
#endif

#if ENABLE_KVSTORE
	struct kvsClient *client; /* KV 模式下关联的高层客户端状态 */
#endif

};

#if ENABLE_HTTP
int http_request(struct conn *c);
int http_response(struct conn *c);
#endif


#if ENABLE_WEBSOCKET
int ws_request(struct conn *c);
int ws_response(struct conn *c);
#endif

#if ENABLE_KVSTORE
int kvs_request(struct conn *c);
int kvs_response(struct conn *c);
int kvs_protocol(char *msg, int length, char *response);
#endif

#endif


