// gcc -o kvstore kvstore.c proactor.c -luring


#include <stdio.h>
#include <liburing.h>
#include <netinet/in.h>
#include <string.h>
#include <unistd.h>

#include "server.h"
#include "../kvstore.h"
#include "../src/client.h"

#define CONNECTION_SIZE		1024

#define EVENT_ACCEPT   	0
#define EVENT_READ		1
#define EVENT_WRITE		2

struct conn_info {
	int fd;
	int event;
};


typedef struct kvsProactorLoop {
	struct io_uring ring;
	int listenfd;
	struct sockaddr_in clientaddr;
	socklen_t addrlen;
} kvsProactorLoop;


static kvsProactorLoop *g_proactor_loop = NULL;

int p_init_server(unsigned short port) {	

	int sockfd = socket(AF_INET, SOCK_STREAM, 0);	
	struct sockaddr_in serveraddr;	
	memset(&serveraddr, 0, sizeof(struct sockaddr_in));	
	serveraddr.sin_family = AF_INET;	
	serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);	
	serveraddr.sin_port = htons(port);	

	if (-1 == bind(sockfd, (struct sockaddr*)&serveraddr, sizeof(struct sockaddr))) {		
		perror("bind");		
		return -1;	
	}	

	listen(sockfd, 10);
	
	return sockfd;
}



#define ENTRIES_LENGTH		1024
#define BUFFER_LENGTH		1024

int set_event_recv(struct io_uring *ring, int sockfd,
			      void *buf, size_t len, int flags) {

	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
	if (!sqe) return -1;

	struct conn_info info = {
		.fd = sockfd,
		.event = EVENT_READ,
	};

	io_uring_prep_recv(sqe, sockfd, buf, len, flags);
	memcpy(&sqe->user_data, &info, sizeof(struct conn_info));
	return 0;
}


int set_event_send(struct io_uring *ring, int sockfd,
			      void *buf, size_t len, int flags) {

	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
	if (!sqe) return -1;

	struct conn_info info = {
		.fd = sockfd,
		.event = EVENT_WRITE,
	};

	io_uring_prep_send(sqe, sockfd, buf, len, flags);
	memcpy(&sqe->user_data, &info, sizeof(struct conn_info));
	return 0;
}



int set_event_accept(struct io_uring *ring, int sockfd, struct sockaddr *addr,
				socklen_t *addrlen, int flags) {

	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
	if (!sqe) return -1;

	struct conn_info info = {
		.fd = sockfd,
		.event = EVENT_ACCEPT,
	};

	io_uring_prep_accept(sqe, sockfd, (struct sockaddr*)addr, addrlen, flags);
	memcpy(&sqe->user_data, &info, sizeof(struct conn_info));
	return 0;
}
static kvsClientHandler kvs_handler;

/* 每个连接对应一个高层 kvsClient 和接收缓冲区，实现与 reactor/ntyco 一致的流式处理。 */
static kvsClient *g_clients[CONNECTION_SIZE] = {0};
static char g_recvbuf[CONNECTION_SIZE][BUFFER_LENGTH];

// 事件掩码
#define KVS_EVENT_READABLE 0x01
#define KVS_EVENT_WRITABLE 0x02

static unsigned char g_events[CONNECTION_SIZE] = {0};

static void kvs_proactor_del_readable(int fd) {
	if (fd < 0 || fd >= CONNECTION_SIZE) return;
	g_events[fd] &= ~KVS_EVENT_READABLE;
}

static void kvs_proactor_del_writable(int fd) {
	if (fd < 0 || fd >= CONNECTION_SIZE) return;
	g_events[fd] &= ~KVS_EVENT_WRITABLE;
}

static void kvs_proactor_add_readable(kvsProactorLoop *loop, int fd) {
	if (!loop) return;
	if (fd < 0 || fd >= CONNECTION_SIZE) return;

	/* 切换为“只读”状态，并提交一次新的 recv */
	g_events[fd] |= KVS_EVENT_READABLE;
	g_events[fd] &= ~KVS_EVENT_WRITABLE;

	set_event_recv(&loop->ring, fd,
		g_recvbuf[fd], BUFFER_LENGTH, 0);
}

static void kvs_proactor_add_writable(kvsProactorLoop *loop, int fd) {
	if (!loop) return;
	if (fd < 0 || fd >= CONNECTION_SIZE) return;
	if (!g_clients[fd]) return;

	if (!g_clients[fd]->replybuf ||
		g_clients[fd]->replylen <= g_clients[fd]->replypos) {
		/* 没有待发送数据，退回读事件 */
		kvs_proactor_add_readable(loop, fd);
		return;
	}

	g_events[fd] |= KVS_EVENT_WRITABLE;
	g_events[fd] &= ~KVS_EVENT_READABLE;

	int remain = g_clients[fd]->replylen - g_clients[fd]->replypos;
	set_event_send(&loop->ring, fd,
		g_clients[fd]->replybuf + g_clients[fd]->replypos,
		(size_t)remain, 0);
}

int proactor_push_reply(int fd, const char *data, int len) {
	if (fd < 0 || fd >= CONNECTION_SIZE) return -1;
	if (!data || len <= 0) return 0;
	if (!g_clients[fd]) return -1;
	if (!g_proactor_loop) return -1;

	if (kvs_client_append_reply(g_clients[fd], data, len) < 0) return -1;

	/* 立即安排写入 */
	kvs_proactor_del_readable(fd);
	kvs_proactor_add_writable(g_proactor_loop, fd);
	return len;
}

static int kvs_proactor_run_loop(kvsProactorLoop *loop) {
	if (!loop) return -1;

	while (1) {

		io_uring_submit(&loop->ring);

		struct io_uring_cqe *cqe;
		io_uring_wait_cqe(&loop->ring, &cqe);

		struct io_uring_cqe *cqes[128];
		int nready = io_uring_peek_batch_cqe(&loop->ring, cqes, 128);  // epoll_wait

		for (int i = 0; i < nready; i++) {

			struct io_uring_cqe *entries = cqes[i];
			struct conn_info result;
			memcpy(&result, &entries->user_data, sizeof(struct conn_info));
			int fd = result.fd;
			int ret = entries->res;

			if (result.event == EVENT_ACCEPT) {

				set_event_accept(&loop->ring, loop->listenfd,
					(struct sockaddr*)&loop->clientaddr, &loop->addrlen, 0);
				int connfd = entries->res;
				if (connfd < 0 || connfd >= CONNECTION_SIZE) {
					printf("proactor: connfd %d out of range, close\n", connfd);
					if (connfd >= 0) close(connfd);
					continue;
				}

				if (kvs_handler) {
					kvsClient *c = kvs_client_create(connfd);
					if (!c) {
						printf("proactor: kvs_client_create failed for fd %d\n", connfd);
						close(connfd);
						continue;
					}
					g_clients[connfd] = c;
				}

				/* 新连接从“读”状态开始 */
				g_events[connfd] = 0;
				kvs_proactor_add_readable(loop, connfd);

			} else if (result.event == EVENT_READ) {

				if (fd < 0 || fd >= CONNECTION_SIZE || !g_clients[fd]) {
					if (fd >= 0) close(fd);
					if (fd >= 0 && fd < CONNECTION_SIZE) {
						g_events[fd] = 0;
					}
					continue;
				}

				if (ret <= 0) {
					printf("proactor: recv ret=%d, close fd %d\n", ret, fd);
					kvs_client_free(g_clients[fd]);
					g_clients[fd] = NULL;
					close(fd);
					g_events[fd] = 0;
					continue;
				}

				/* 将收到的数据追加到客户端输入缓冲区，并处理尽可能多的命令 */
				kvs_client_append_query(g_clients[fd], g_recvbuf[fd], ret);
				kvs_handler(g_clients[fd]);

				if (g_clients[fd]->replybuf &&
					g_clients[fd]->replylen > g_clients[fd]->replypos) {
					/* 有待发送数据，从读切换到写 */
					kvs_proactor_del_readable(fd);
					kvs_proactor_add_writable(loop, fd);
				} else {
					/* 没有待发送数据，保持或恢复读状态 */
					kvs_proactor_del_writable(fd);
					kvs_proactor_add_readable(loop, fd);
				}

			} else if (result.event == EVENT_WRITE) {

				if (fd < 0 || fd >= CONNECTION_SIZE || !g_clients[fd]) {
					if (fd >= 0) close(fd);
					if (fd >= 0 && fd < CONNECTION_SIZE) {
						g_events[fd] = 0;
					}
					continue;
				}

				if (ret <= 0) {
					printf("proactor: send ret=%d, close fd %d\n", ret, fd);
					kvs_client_free(g_clients[fd]);
					g_clients[fd] = NULL;
					close(fd);
					g_events[fd] = 0;
					continue;
				}

				g_clients[fd]->replypos += ret;
				if (g_clients[fd]->replypos < g_clients[fd]->replylen) {
					/* 仍有剩余数据，继续保持写状态 */
					kvs_proactor_add_writable(loop, fd);
				} else {
					/* 本轮所有数据已发送完，重置计数并继续接收 */
					g_clients[fd]->replylen = 0;
					g_clients[fd]->replypos = 0;
					kvs_proactor_del_writable(fd);
					kvs_proactor_add_readable(loop, fd);
				}

			}
		}

		io_uring_cq_advance(&loop->ring, nready);
	}

	return 0;
}

int proactor_start(unsigned short port, kvsClientHandler handler) {

	int sockfd = p_init_server(port);
	kvs_handler = handler;

	struct io_uring_params params;
	memset(&params, 0, sizeof(params));

	kvsProactorLoop loop;
	memset(&loop, 0, sizeof(loop));
	loop.listenfd = sockfd;
	loop.addrlen = sizeof(loop.clientaddr);
	g_proactor_loop = &loop;

	io_uring_queue_init_params(ENTRIES_LENGTH, &loop.ring, &params);

	/* 初始化首个 accept 事件 */
	set_event_accept(&loop.ring, loop.listenfd,
		(struct sockaddr*)&loop.clientaddr, &loop.addrlen, 0);

	return kvs_proactor_run_loop(&loop);
}


