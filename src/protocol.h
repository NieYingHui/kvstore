#ifndef KVS_PROTOCOL_H
#define KVS_PROTOCOL_H

#include "../kvstore.h"

typedef int (*kvsProtocolAppendFn)(const char *data, int len, void *ud);

/*
 * kvs_protocol_stream:
 *   处理输入缓冲区中的一批命令，并通过 append 回调按需追加所有回复。
 *   不再假设有固定大小的输出缓冲区，便于支持大 value 和流式输出。
 */
int kvs_protocol_stream(char *msg, int length, kvsProtocolAppendFn append, void *ud);

int kvs_protocol(char *msg, int length, char *response);
/*
 * kvs_protocol_last_consumed:
 *   返回最近一次调用实际消耗的输入字节数，
 *   便于上层在存在粘包/半包的场景下做流式拼接和残留数据保留。
 */
int kvs_protocol_last_consumed(void);

#endif 
