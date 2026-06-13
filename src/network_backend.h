#ifndef KVS_NETWORK_BACKEND_H
#define KVS_NETWORK_BACKEND_H


#define NETWORK_REACTOR        0
#define NETWORK_PROACTOR       1
#define NETWORK_NTYCO          2

#ifndef NETWORK_SELECT
#define NETWORK_SELECT         NETWORK_REACTOR
#endif

#include "client.h"

int reactor_start(unsigned short port, kvsClientHandler handler);
int proactor_start(unsigned short port, kvsClientHandler handler);
int ntyco_start(unsigned short port, kvsClientHandler handler);

/* 将数据追加到指定 fd 的 replybuf，并尽快触发发送（用于复制增量推送）
 * 返回追加的字节数，<0 表示失败。
 */
int kvs_backend_push_reply(int fd, const char *data, int len);

/* 由各网络后端提供的实现（内部使用）。 */
int reactor_push_reply(int fd, const char *data, int len);
int proactor_push_reply(int fd, const char *data, int len);
int ntyco_push_reply(int fd, const char *data, int len);

#endif
