#include <stdio.h>

#include "../kvstore.h"
#include "engine.h"
#include "protocol.h"
#include "client.h"
#include "net_server.h"

int kvs_server_run(unsigned short port) {
    if (init_kvengine() != 0) {
        fprintf(stderr, "Failed to init kv engine\n");
        return -1;
    }

    if (kvs_net_server_start(port, kvs_process_client) != 0) {
        destroy_kvengine();
        return -1;
    }

    destroy_kvengine();
    return 0;
}
