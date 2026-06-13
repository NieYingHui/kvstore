#ifndef __REPLICATOR_H__
#define __REPLICATOR_H__


int replicator_init_master(const char *host, unsigned short port);
void replicator_close_master();
int replicator_send_request(const char *req, int len, char *resp, int resp_buf_len);

int replicator_init_slave(unsigned short port); // starts listener thread
void replicator_stop_slave();


#endif