#ifndef KVS_EVENTLOOP_H
#define KVS_EVENTLOOP_H

/*
 * 事件循环抽象骨架：
 * 目前只提供类型和接口声明，不改动现有 reactor/proactor/ntyco 逻辑。
 * 后续会分别在各自实现文件中按这些接口做适配。
 */

#include "net_connection.h"
#include "client.h"

typedef struct kvsEventLoop kvsEventLoop;

typedef int (*kvsConnCallback)(kvsConnection *conn);

/* 创建 / 销毁事件循环实例（具体实现由不同后端提供） */
kvsEventLoop *kvs_eventloop_create(int setsize);
void kvs_eventloop_delete(kvsEventLoop *loop);

/* 注册 / 取消关注读写事件 */
int kvs_eventloop_add_readable(kvsEventLoop *loop, kvsConnection *conn, kvsConnCallback cb);
int kvs_eventloop_add_writable(kvsEventLoop *loop, kvsConnection *conn, kvsConnCallback cb);
int kvs_eventloop_del_readable(kvsEventLoop *loop, kvsConnection *conn);
int kvs_eventloop_del_writable(kvsEventLoop *loop, kvsConnection *conn);

/* 运行事件循环 */
int kvs_eventloop_run(kvsEventLoop *loop);

/*
 * 统一的服务器事件循环入口：
 * 根据全局配置选择底层网络后端（reactor/proactor/ntyco），
 * 并运行相应的事件循环实现。
 */
int kvs_eventloop_run_server(unsigned short port, kvsClientHandler handler);

#endif
