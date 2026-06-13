// gcc -o kvstore kvstore.c reactor.c


#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <poll.h>
#include <sys/epoll.h>
#include <errno.h>
#include <sys/time.h>

#include "../kvstore.h"
#include "server.h"
#include "../src/eventloop.h"


#define CONNECTION_SIZE			1024 // 1024 * 1024

#define MAX_PORTS			20

#define TIME_SUB_MS(tv1, tv2)  ((tv1.tv_sec - tv2.tv_sec) * 1000 + (tv1.tv_usec - tv2.tv_usec) / 1000)

struct kvsEventLoop {
	int epfd;
	int setsize;
};

static kvsEventLoop *g_event_loop = NULL;

#if ENABLE_KVSTORE

#include "../src/client.h"

static kvsClientHandler kvs_handler;

#endif


int accept_cb(int fd);
int recv_cb(int fd);
int send_cb(int fd);


int epfd = 0;
struct timeval begin;


struct conn conn_list[CONNECTION_SIZE] = {0};
// fd

int reactor_push_reply(int fd, const char *data, int len) {
	if (fd < 0 || fd >= CONNECTION_SIZE) return -1;
	if (!data || len <= 0) return 0;
	if (!conn_list[fd].client) return -1;
	if (kvs_client_append_reply(conn_list[fd].client, data, len) < 0) return -1;

	if (g_event_loop) {
		kvs_eventloop_del_readable(g_event_loop, &conn_list[fd]);
		kvs_eventloop_add_writable(g_event_loop, &conn_list[fd], NULL);
	}
	return len;
}

kvsEventLoop *kvs_eventloop_create(int setsize) {
	kvsEventLoop *loop = (kvsEventLoop *)calloc(1, sizeof(kvsEventLoop));
	if (!loop) return NULL;

	int efd = epoll_create(1);
	if (efd < 0) {
		free(loop);
		return NULL;
	}
	loop->epfd = efd;
	loop->setsize = setsize > 0 ? setsize : CONNECTION_SIZE;

	/* 同步到原有的全局 epfd，并记录全局事件循环指针 */
	epfd = efd;
	g_event_loop = loop;
	return loop;
}

void kvs_eventloop_delete(kvsEventLoop *loop) {
	if (!loop) return;
	if (loop->epfd >= 0) {
		close(loop->epfd);
	}
	if (g_event_loop == loop) {
		g_event_loop = NULL;
	epfd = 0;
	}
	free(loop);
}

/* 使用 conn.status 保存当前关注的读/写事件位 */
#define KVS_EVENT_READABLE 0x01
#define KVS_EVENT_WRITABLE 0x02

static int kvs_eventloop_update_events(int fd, int oldmask, int newmask) {
	struct epoll_event ev;
	int op;

	if (newmask == 0) {
		/* 没有任何事件需要关注，从 epoll 中移除 */
		epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
		return 0;
	}

	ev.events = 0;
	if (newmask & KVS_EVENT_READABLE) ev.events |= EPOLLIN;
	if (newmask & KVS_EVENT_WRITABLE) ev.events |= EPOLLOUT;
	ev.data.fd = fd;

	if (oldmask == 0) {
		op = EPOLL_CTL_ADD;
	} else {
		op = EPOLL_CTL_MOD;
	}

	return epoll_ctl(epfd, op, fd, &ev);
}

int kvs_eventloop_add_readable(kvsEventLoop *loop, kvsConnection *conn, kvsConnCallback cb) {
	(void)loop;
	(void)cb;
	if (!conn) return -1;
	int fd = conn->fd;
	int oldmask = conn_list[fd].status;
	conn_list[fd].status |= KVS_EVENT_READABLE;
	int newmask = conn_list[fd].status;
	return kvs_eventloop_update_events(fd, oldmask, newmask);
}

int kvs_eventloop_add_writable(kvsEventLoop *loop, kvsConnection *conn, kvsConnCallback cb) {
	(void)loop;
	(void)cb;
	if (!conn) return -1;
	int fd = conn->fd;
	int oldmask = conn_list[fd].status;
	conn_list[fd].status |= KVS_EVENT_WRITABLE;
	int newmask = conn_list[fd].status;
	return kvs_eventloop_update_events(fd, oldmask, newmask);
}

int kvs_eventloop_del_readable(kvsEventLoop *loop, kvsConnection *conn) {
	(void)loop;
	if (!conn) return -1;
	int fd = conn->fd;
	int oldmask = conn_list[fd].status;
	conn_list[fd].status &= ~KVS_EVENT_READABLE;
	int newmask = conn_list[fd].status;
	return kvs_eventloop_update_events(fd, oldmask, newmask);
}

int kvs_eventloop_del_writable(kvsEventLoop *loop, kvsConnection *conn) {
	(void)loop;
	if (!conn) return -1;
	int fd = conn->fd;
	int oldmask = conn_list[fd].status;
	conn_list[fd].status &= ~KVS_EVENT_WRITABLE;
	int newmask = conn_list[fd].status;
	return kvs_eventloop_update_events(fd, oldmask, newmask);
}

int kvs_eventloop_run(kvsEventLoop *loop) {
	if (!loop) return -1;

	gettimeofday(&begin, NULL);

	while (1) { // mainloop

		struct epoll_event events[1024] = {0};
		int nready = epoll_wait(loop->epfd, events, 1024, -1);

		int i = 0;
		for (i = 0; i < nready; i++) {

			int connfd = events[i].data.fd;

#if 0
			if (events[i].events & EPOLLIN) {
				conn_list[connfd].r_action.recv_callback(connfd);
			} else if (events[i].events & EPOLLOUT) {
				conn_list[connfd].send_callback(connfd);
			}

#else 
			if (events[i].events & EPOLLIN) {
				conn_list[connfd].r_action.recv_callback(connfd);
			} 

			if (events[i].events & EPOLLOUT) {
				conn_list[connfd].send_callback(connfd);
			}
#endif
		}

	}

	return 0;
}
int event_register(int fd, int event) {

	if (fd < 0) return -1;

	conn_list[fd].fd = fd;
	conn_list[fd].r_action.recv_callback = recv_cb;
	conn_list[fd].send_callback = send_cb;

	memset(conn_list[fd].rbuffer, 0, BUFFER_LENGTH);
	conn_list[fd].rlength = 0;

	memset(conn_list[fd].wbuffer, 0, BUFFER_LENGTH);
	conn_list[fd].wlength = 0;
	conn_list[fd].status = 0;

#if ENABLE_KVSTORE
	conn_list[fd].client = NULL;
#endif

	if (event & EPOLLIN) {
		kvs_eventloop_add_readable(g_event_loop, &conn_list[fd], NULL);
	}
	if (event & EPOLLOUT) {
		kvs_eventloop_add_writable(g_event_loop, &conn_list[fd], NULL);
	}
	return 0;
}


// listenfd(sockfd) --> EPOLLIN --> accept_cb
int accept_cb(int fd) {

	struct sockaddr_in  clientaddr;
	socklen_t len = sizeof(clientaddr);

	int clientfd = accept(fd, (struct sockaddr*)&clientaddr, &len);
	//printf("accept finshed: %d\n", clientfd);
	if (clientfd < 0) {
		printf("accept errno: %d --> %s\n", errno, strerror(errno));
		return -1;
	}
	
	event_register(clientfd, EPOLLIN);  // | EPOLLET

#if ENABLE_KVSTORE
	if (kvs_handler) {
		kvsClient *c = kvs_client_create(clientfd);
		if (!c) {
			printf("kvs_client_create failed for fd %d\n", clientfd);
			close(clientfd);
			return -1;
		}
		conn_list[clientfd].client = c;
	}
#endif

	if ((clientfd % 1000) == 0) {

		struct timeval current;
		gettimeofday(&current, NULL);

		int time_used = TIME_SUB_MS(current, begin);
		memcpy(&begin, &current, sizeof(struct timeval));
		

		printf("accept finshed: %d, time_used: %d\n", clientfd, time_used);

	}

	return 0;
}


int recv_cb(int fd) {

	memset(conn_list[fd].rbuffer, 0, BUFFER_LENGTH );
	int count = recv(fd, conn_list[fd].rbuffer, BUFFER_LENGTH, 0);
	if (count == 0) { // disconnect
		printf("client disconnect: %d\n", fd);

#if ENABLE_KVSTORE
		if (conn_list[fd].client) {
			kvs_client_free(conn_list[fd].client);
			conn_list[fd].client = NULL;
		}
#endif
		epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL); // unfinished
		close(fd);

		

		return 0;
	} else if (count < 0) { // recv error

		/*
		 * 压测/客户端退出时常见：
		 *   - ECONNRESET: 对端直接 RST 关闭（redis-benchmark 结束时快速退出很常见）
		 *   - ECONNABORTED: 连接被中止
		 * 这类情况本质等价于“断开连接”，不需要当作服务端错误刷屏。
		 */
		if (errno != ECONNRESET && errno != ECONNABORTED && errno != EPIPE) {
			printf("count: %d, errno: %d, %s\n", count, errno, strerror(errno));
		}

#if ENABLE_KVSTORE
		if (conn_list[fd].client) {
			kvs_client_free(conn_list[fd].client);
			conn_list[fd].client = NULL;
		}
#endif

		epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL); // unfinished	
		close(fd);
		

		return 0;
	}

	conn_list[fd].rlength = count;

#if 0 // echo
	conn_list[fd].wlength = conn_list[fd].rlength;
	memcpy(conn_list[fd].wbuffer, conn_list[fd].rbuffer, conn_list[fd].wlength);
	printf("[%d]RECV: %s\n", conn_list[fd].rlength, conn_list[fd].rbuffer);

#elif ENABLE_HTTP
	http_request(&conn_list[fd]);

#elif ENABLE_WEBSOCKET
	ws_request(&conn_list[fd]);

#elif ENABLE_KVSTORE
	if (conn_list[fd].client && kvs_handler) {
		/* 追加到高层 querybuf 并处理尽可能多的完整命令 */
		kvs_client_append_query(conn_list[fd].client,
			conn_list[fd].rbuffer, conn_list[fd].rlength);
		kvs_handler(conn_list[fd].client);
	}

#endif

#if ENABLE_KVSTORE
	if (conn_list[fd].client &&
		conn_list[fd].client->replybuf &&
		conn_list[fd].client->replylen > conn_list[fd].client->replypos) {
		/* 从读切换到写，只关注可写事件 */
		kvs_eventloop_del_readable(g_event_loop, &conn_list[fd]);
		kvs_eventloop_add_writable(g_event_loop, &conn_list[fd], NULL);
	} else {
		/* 继续只关注可读事件 */
		kvs_eventloop_del_writable(g_event_loop, &conn_list[fd]);
		kvs_eventloop_add_readable(g_event_loop, &conn_list[fd], NULL);
	}
#else
	/* 非 KV 模式，收到数据后切换为只写 */
	kvs_eventloop_del_readable(g_event_loop, &conn_list[fd]);
	kvs_eventloop_add_writable(g_event_loop, &conn_list[fd], NULL);
#endif

	return count;
}


int send_cb(int fd) {
	int count = 0;

#if ENABLE_KVSTORE
	if (conn_list[fd].client &&
		conn_list[fd].client->replybuf &&
		conn_list[fd].client->replylen > conn_list[fd].client->replypos) {
		int remain = conn_list[fd].client->replylen - conn_list[fd].client->replypos;
		count = send(fd,
			conn_list[fd].client->replybuf + conn_list[fd].client->replypos,
			remain, 0);
		if (count > 0) {
			conn_list[fd].client->replypos += count;
		} else {
			/*
			 * 发送错误或对端关闭：
			 *  - EPIPE/ECONNRESET 等在压测结束或客户端退出时很常见
			 * 这里直接回收连接即可，避免误报。
			 */
			if (errno != EPIPE && errno != ECONNRESET && errno != ECONNABORTED) {
				printf("send error on fd %d (errno=%d %s), close it\n", fd, errno, strerror(errno));
			}
			kvs_client_free(conn_list[fd].client);
			conn_list[fd].client = NULL;
			close(fd);
			epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
			return 0;
		}
	}

	if (!conn_list[fd].client ||
		conn_list[fd].client->replylen <= conn_list[fd].client->replypos) {
		/* 所有待发送数据已经发送完，重置回复缓冲区 */
		if (conn_list[fd].client) {
			conn_list[fd].client->replylen = 0;
			conn_list[fd].client->replypos = 0;
		}
		/* 发送完成，恢复为只读 */
		kvs_eventloop_del_writable(g_event_loop, &conn_list[fd]);
		kvs_eventloop_add_readable(g_event_loop, &conn_list[fd], NULL);
	} else {
		/* 仍有剩余数据，继续只关注可写事件（状态已是写，不必重复设置） */
	}

    return count;

#else

#if ENABLE_HTTP
	http_response(&conn_list[fd]);
#elif ENABLE_WEBSOCKET
	ws_response(&conn_list[fd]);
#endif

	if (conn_list[fd].wlength != 0) {
		count = send(fd, conn_list[fd].wbuffer, conn_list[fd].wlength, 0);
	}
	/* 发送完成，恢复为只读 */
	kvs_eventloop_del_writable(g_event_loop, &conn_list[fd]);
	kvs_eventloop_add_readable(g_event_loop, &conn_list[fd], NULL);
	return count;

#endif
}



int r_init_server(unsigned short port) {

	int sockfd = socket(AF_INET, SOCK_STREAM, 0);

	struct sockaddr_in servaddr;
	servaddr.sin_family = AF_INET;
	servaddr.sin_addr.s_addr = htonl(INADDR_ANY); // 0.0.0.0
	servaddr.sin_port = htons(port); // 0-1023, 

	if (-1 == bind(sockfd, (struct sockaddr*)&servaddr, sizeof(struct sockaddr))) {
		printf("bind failed: %s\n", strerror(errno));
	}

	listen(sockfd, 10);
	//printf("listen finshed: %d\n", sockfd); // 3 

	return sockfd;

}

int reactor_start(unsigned short port, kvsClientHandler handler) {

	kvs_handler = handler;

	kvsEventLoop *loop = kvs_eventloop_create(CONNECTION_SIZE);
	if (!loop) {
		printf("kvs_eventloop_create failed\n");
		return -1;
	}

	int i = 0;

	for (i = 0;i < MAX_PORTS;i ++) {
		
		int sockfd = r_init_server(port + i);
		
		conn_list[sockfd].fd = sockfd;
		conn_list[sockfd].r_action.recv_callback = accept_cb;
		conn_list[sockfd].status = 0;
		/* 监听套接字只关注可读（accept）事件 */
		kvs_eventloop_add_readable(loop, &conn_list[sockfd], NULL);
	}

	/* 使用统一的事件循环接口运行主循环 */
	return kvs_eventloop_run(loop);
}


