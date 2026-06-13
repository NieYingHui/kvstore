#ifndef KVS_NET_SERVER_H
#define KVS_NET_SERVER_H

#include "../kvstore.h"
#include "client.h"


int kvs_net_server_start(unsigned short port, kvsClientHandler handler);

#endif
