#ifndef KVS_RESP_H
#define KVS_RESP_H

#include "protocol.h"


int kvs_resp_append_bulk_len(kvsProtocolAppendFn append, void *ud,
                            const char *data, int len);

int kvs_resp_append_array_argv(kvsProtocolAppendFn append, void *ud,
                              int argc, const char * const *argv);

#endif
