#ifndef _KVS_AOF_H_
#define _KVS_AOF_H_

/*
 * AOF (Append Only File) module.
 */

int kvs_aof_init(void);
int kvs_aof_append_argv(int argc, const char * const *argv);
int kvs_aof_recover(void);
int kvs_aof_bgrewriteaof_start(void);
void kvs_aof_close(void);

#endif
