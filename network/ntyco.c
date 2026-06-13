// gcc -o kvstore kvstore.c ntyco.c -I NtyCo/core/ -L ./NtyCo/ -lntyco



#include "nty_coroutine.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../src/client.h"
#include "../src/network_backend.h"

typedef struct kvsCoLoop {
	unsigned short port;
} kvsCoLoop;

static kvsClientHandler kvs_handler;


#define CO_CONNECTION_SIZE 1024
#define KVS_EVENT_READABLE 0x01
#define KVS_EVENT_WRITABLE 0x02

static unsigned char g_co_events[CO_CONNECTION_SIZE] = {0};

static kvsClient *g_clients[CO_CONNECTION_SIZE] = {0};
static unsigned char g_sending[CO_CONNECTION_SIZE] = {0};

static int kvs_ntyco_sendall(int fd, const char *buf, int len) {
	int sent = 0;
	while (sent < len) {
		int n = send(fd, buf + sent, len - sent, 0);
		if (n > 0) {
			sent += n;
			continue;
		}
		if (n == 0) {
			return -1;
		}
		if (errno == EINTR) {
			continue;
		}
		return -1;
	}
	return 0;
}

static int kvs_ntyco_flush_reply(int fd) {
	if (fd < 0 || fd >= CO_CONNECTION_SIZE) return -1;
	kvsClient *client = g_clients[fd];
	if (!client) return -1;
	if (!client->replybuf || client->replylen <= client->replypos) {
		client->replylen = 0;
		client->replypos = 0;
		return 0;
	}

	g_sending[fd] = 1;
	while (client->replybuf && client->replylen > client->replypos) {
		int remain = client->replylen - client->replypos;
		if (kvs_ntyco_sendall(fd, client->replybuf + client->replypos, remain) != 0) {
			g_sending[fd] = 0;
			return -1;
		}
		client->replypos = client->replylen;
	}
	client->replylen = 0;
	client->replypos = 0;
	g_sending[fd] = 0;
	return 0;
}

static void kvs_co_set_readable(int fd) {
	if (fd < 0 || fd >= CO_CONNECTION_SIZE) return;
	g_co_events[fd] |= KVS_EVENT_READABLE;
	g_co_events[fd] &= ~KVS_EVENT_WRITABLE;
}

static void kvs_co_set_writable(int fd) {
	if (fd < 0 || fd >= CO_CONNECTION_SIZE) return;
	g_co_events[fd] |= KVS_EVENT_WRITABLE;
	g_co_events[fd] &= ~KVS_EVENT_READABLE;
}

static void kvs_co_clear_events(int fd) {
	if (fd < 0 || fd >= CO_CONNECTION_SIZE) return;
	g_co_events[fd] = 0;
}

void server_reader(void *arg) {
	if (!arg) return;
	int fd = *(int *)arg;
	free(arg);
	int ret = 0;
	if (fd < 0 || fd >= CO_CONNECTION_SIZE) {
		close(fd);
		return;
	}
	kvsClient *client = kvs_client_create(fd);
	if (!client) {
		close(fd);
		return;
	}
	g_clients[fd] = client;

	/* 新连接初始为读状态 */
	kvs_co_set_readable(fd);

	while (1) {
		char buf[1024] = {0};
		ret = recv(fd, buf, 1024, 0);
		if (ret > 0) {
			/* 将收到的数据追加到客户端缓冲区并处理 */
			kvs_client_append_query(client, buf, ret);
			kvs_handler(client);

			if (client->replybuf && client->replylen > client->replypos) {
				kvs_co_set_writable(fd);
				if (kvs_ntyco_flush_reply(fd) != 0) {
					close(fd);
					kvs_client_free(client);
					g_clients[fd] = NULL;
					kvs_co_clear_events(fd);
					return;
				}
				kvs_co_set_readable(fd);
			} else {
				kvs_co_set_readable(fd);
			}
		} else {
			close(fd);
			kvs_client_free(client);
			g_clients[fd] = NULL;
			kvs_co_clear_events(fd);
			return;
		}
	}
}

int ntyco_push_reply(int fd, const char *data, int len) {
	if (fd < 0 || fd >= CO_CONNECTION_SIZE) return -1;
	if (!data || len <= 0) return 0;
	kvsClient *client = g_clients[fd];
	if (!client) return -1;
	if (kvs_client_append_reply(client, data, len) < 0) return -1;

	/* 如果该 fd 正在发送，只追加即可，保持字节序。 */
	if (g_sending[fd]) {
		return len;
	}

	/* 尝试立即 flush，确保 replica 在线时能收到增量命令。 */
	if (kvs_ntyco_flush_reply(fd) != 0) {
		return -1;
	}
	return len;
}

void server(void *arg) {
	kvsCoLoop *loop = (kvsCoLoop *)arg;
	unsigned short port = loop->port;

	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return;

	struct sockaddr_in local, remote;
	local.sin_family = AF_INET;
	local.sin_port = htons(port);
	local.sin_addr.s_addr = INADDR_ANY;
	bind(fd, (struct sockaddr*)&local, sizeof(struct sockaddr_in));

	listen(fd, 20);
	printf("listen port : %d\n", port);

	while (1) {
		socklen_t len = sizeof(struct sockaddr_in);
		int cli_fd = accept(fd, (struct sockaddr*)&remote, &len);
		if (cli_fd < 0) {
			continue;
		}
		int *pfd = (int *)malloc(sizeof(int));
		if (!pfd) {
			close(cli_fd);
			continue;
		}
		*pfd = cli_fd;

		nty_coroutine *read_co;
		nty_coroutine_create(&read_co, server_reader, pfd);
 	}
}

static int kvs_co_run_loop(kvsCoLoop *loop) {
	nty_coroutine *co = NULL;
	nty_coroutine_create(&co, server, loop);

	nty_schedule_run();
	return 0;
}

int ntyco_start(unsigned short port, kvsClientHandler handler) {
	kvs_handler = handler;

	kvsCoLoop loop;
	loop.port = port;

	return kvs_co_run_loop(&loop);
}




