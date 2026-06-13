#include <stdio.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>

#include "../kvstore.h"
#include "server.h"

static int master_sock = -1;
static int slave_listen_sock = -1;
static pthread_t slave_thread;
static int slave_thread_running = 0;


static int recv_all(int fd, void *buf, int len);
static int send_all(int fd, const void *buf, int len);

int replicator_init_master(const char *host, unsigned short port);
void replicator_close_master();
int replicator_send_request(const char *req, int len, char *resp, int resp_buf_len);

static int handle_master_connection(int fd);
static void *slave_accept_loop(void *arg);
int replicator_init_slave(unsigned short port);
void replicator_stop_slave();



static int send_all(int fd, const void *buf, int len) {
    if(!buf || fd < 0 || len < 0) return -1;

    const char *p = buf;
    int sent = 0;
    while(sent < len) {
        int ret = send(fd, p + sent, len - sent, 0);
        if(ret <= 0) return -1;
        sent += ret;
    }
    return sent;
}


static int recv_all(int fd, void *buf, int len) {
    if(!buf || fd < 0 || len < 0) return -1;

    char *p = buf;
    int recvd = 0;
    while(recvd < len) {
        int ret = recv(fd, p + recvd, len - recvd, 0);
        if(ret <= 0) return -1;
        recvd += ret;
    }

    return recvd;
}

int replicator_init_master(const char *host, unsigned short port) {
    struct sockaddr_in addr;
    int sock = socket(AF_INET, SOCK_STREAM, 0);

    if(sock < 0) return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    // 使用inet_pton()将点分十进制IP地址字符串转换为网络
    if(inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        close(sock);
        return -1;
    }

    if(connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }

    master_sock = sock;
    return 0;
}


void replicator_close_master() {
    if(master_sock >= 0) close(master_sock);
    master_sock = -1;
}

int replicator_send_request(const char *req, int len, char *resp, int resp_buf_len) {
    if(master_sock < 0) return -1;

    uint32_t l = htonl(len);// 先发送长度
    if(send_all(master_sock, &l, sizeof(l)) != sizeof(l)) return -1;

    if(len > 0) if(send_all(master_sock, req, len) != len) return -1; // 在发送消息

    uint32_t rlen_net;
    if(recv_all(master_sock, &rlen_net, sizeof(rlen_net)) != sizeof(rlen_net)) return -1;

    uint32_t rlen = ntohl(rlen_net);
    if((int)rlen > resp_buf_len) return -1;
    if(rlen > 0) if(recv_all(master_sock, resp, rlen) != (int)rlen) return -1;

    return rlen;
}


static int handle_master_connection(int fd) {
    while(1) {
        uint32_t rlen_net;
        int n = recv_all(fd, &rlen_net, sizeof(rlen_net));
        if(n <= 0) break;

        uint32_t rlen = ntohl(rlen_net);
        if(rlen == 0){
            // empty request
            uint32_t zero = 0;
            uint32_t zero_net = htonl(zero);
            send_all(fd, &zero_net, sizeof(zero_net));
            continue;
        }

        char *req = kvs_malloc(rlen + 1);
        if(!req) break;

        if(recv_all(fd, req, rlen) != (int)rlen) { kvs_free(req); break;}
        req[rlen] = '\0';

        char resp[BUFFER_LENGTH] = {0};
        int r = kvs_protocol(req, rlen, resp);
        kvs_free(req);

        uint32_t rlen_out = (r > 0) ? r : 0;
        uint32_t rlen_out_net = htonl(rlen_out);
        if(send_all(fd, &rlen_out_net, sizeof(rlen_out_net)) != sizeof(rlen_out_net)) break;
        if(rlen_out >0) if(send_all(fd, resp, rlen_out) != (int)rlen_out) break;

    }
    return 0;
}


static void *slave_accept_loop(void *arg) {
    (void)arg;

    while(slave_thread_running) {
        struct sockaddr_in cli;
        socklen_t len = sizeof(cli);

        int fd = accept(slave_listen_sock, (struct sockaddr*)&cli, &len);
        if(fd < 0) continue;

        handle_master_connection(fd);
        close(fd);
    }
    return NULL;
}


int replicator_init_slave(unsigned short port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if(sock < 0) return -1;

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if(bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }

    if(listen(sock, 1) < 0) {
        close(sock);
        return -1;
    }

    slave_listen_sock = sock;
    slave_thread_running = 1;
    pthread_create(&slave_thread, NULL, slave_accept_loop, NULL);
    return 0;
}

void replicator_stop_slave() {
    slave_thread_running = 0;
    if(slave_listen_sock >= 0) close(slave_listen_sock);
    slave_listen_sock = -1;
}
