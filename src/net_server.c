#include <stdio.h>

#include "net_server.h"
#include "eventloop.h"

int kvs_net_server_start(unsigned short port, kvsClientHandler handler) {
	/* 统一的事件循环入口 */
	return kvs_eventloop_run_server(port, handler);
}
