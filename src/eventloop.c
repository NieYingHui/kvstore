#include <stdio.h>

#include "eventloop.h"
#include "engine.h"
#include "network_backend.h"

/*
 * 统一的服务器事件循环入口：
 * 由上层传入端口和 kvsClientHandler，
 * 在此根据全局配置选择具体网络后端并启动相应事件循环。
 */
int kvs_eventloop_run_server(unsigned short port, kvsClientHandler handler) {
	if (!handler) {
		fprintf(stderr, "kvs_eventloop_run_server: handler is NULL\n");
		return -1;
	}

	int backend = kvs_server.config.network_backend;
	if (backend < NETWORK_REACTOR || backend > NETWORK_NTYCO) {
		backend = NETWORK_SELECT;
	}

	switch (backend) {
	case NETWORK_REACTOR:
		return reactor_start(port, handler);
	case NETWORK_PROACTOR:
		return proactor_start(port, handler);
	case NETWORK_NTYCO:
		return ntyco_start(port, handler);
	default:
		fprintf(stderr, "kvs_eventloop_run_server: invalid backend %d\n", backend);
		return -1;
	}
}
