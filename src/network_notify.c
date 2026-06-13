#include "network_backend.h"
#include "engine.h"

int kvs_backend_push_reply(int fd, const char *data, int len) {
	int backend = kvs_server.config.network_backend;
	if (backend < NETWORK_REACTOR || backend > NETWORK_NTYCO) {
		backend = NETWORK_SELECT;
	}

	switch (backend) {
		case NETWORK_REACTOR:
			return reactor_push_reply(fd, data, len);
		case NETWORK_PROACTOR:
			return proactor_push_reply(fd, data, len);
		case NETWORK_NTYCO:
			return ntyco_push_reply(fd, data, len);
		default:
			return reactor_push_reply(fd, data, len);
	}
}
