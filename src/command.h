#ifndef KVS_COMMAND_H
#define KVS_COMMAND_H

#include "protocol.h"

typedef int (*kvsCommandProc)(int argc, char **argv, char *response_buf, int *is_write_cmd, int is_resp);

typedef struct kvsCommand {
	const char *name;
	int arity;   
	int flags;   
	kvsCommandProc proc;
} kvsCommand;

const kvsCommand *kvs_lookup_command(const char *name);

#endif 
