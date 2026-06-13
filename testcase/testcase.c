

#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/select.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#define MAX_MSG_LENGTH      1024

#define MAX_MSG_LENGTH		1024
#define TIME_SUB_MS(tv1, tv2)  ((tv1.tv_sec - tv2.tv_sec) * 1000 + (tv1.tv_usec - tv2.tv_usec) / 1000)

int recv_msg(int connfd, char *msg, int length);
int send_msg(int connfd, char *msg, int length);
int testcase(int connfd, char *msg, char *result, char *testname);
int connect_tcpserver(const char *ip, unsigned short port);
void array_testcase(int connfd);
void rbtree_testcase(int connfd);
void hash_testcase(int connfd);
void skiplist_testcase(int connfd);
int array_testcase_1w(int connfd);
int rbtree_testcase_1w(int connfd);
int hash_testcase_1w(int connfd);
int skiplist_testcase_1w(int connfd);
int hash_testcase_100w(int connfd);
int batch_testcase(int connfd, int n);

int connect_tcpserver(const char *ip, unsigned short port) {

	int connfd = socket(AF_INET, SOCK_STREAM, 0);

	struct sockaddr_in tcpserver_addr;
	memset(&tcpserver_addr, 0, sizeof(struct sockaddr_in));

	tcpserver_addr.sin_family = AF_INET;
	tcpserver_addr.sin_addr.s_addr = inet_addr(ip);// 这行代码将字符串形式的IP地址（如"192.168.1.1"）转换为网络字节序的32位二进制格式
	// 例如："192.168.1.1"会被转换为0x0101a8c0（网络字节序）
    tcpserver_addr.sin_port = htons(port);

	int ret = connect(connfd, (struct sockaddr*)&tcpserver_addr, sizeof(struct sockaddr_in));
	if (ret) {
		perror("connect");
		return -1;
	}

	return connfd;
}


int recv_msg(int connfd, char *msg, int length){

    int ret = recv(connfd, msg, length, 0);
    if(ret < 0){
        perror("recv error");
        exit(1);
    }
    return ret;
}

int send_msg(int connfd, char *msg, int length){

    int ret = send(connfd, msg, length, 0);
    if(ret < 0){
        perror("send error");
        exit(1);
    }
    return ret;
}

int testcase(int connfd, char *msg, char *result, char *testname){

    if(!msg || !result || !testname) return -1;

    send_msg(connfd, msg, strlen(msg));

    char results[MAX_MSG_LENGTH] = {0};

    recv_msg(connfd, results, MAX_MSG_LENGTH);

    if(strcmp(results, result) == 0){
        // printf("--> PASS --> %s\n", testname);
    }
    else {
        printf("--> NO PASS --> %s  '%s' != '%s'\n", testname, results, result);
        printf("msg: %s\n", msg);
        exit(1);
    }
    
    return 0;
}
int batch_testcase(int connfd, int n){
    // 连续发送50条命令，期待服务端批量返回50条OK\r\n
    const int batch_size = 50;
    char cmd[128] = {0};
    char expected[256] = {0};
    int expected_len = 0;

    memset(cmd, 0, sizeof(cmd));
    memset(expected, 0, sizeof(expected));

    for (int i = 0; i < batch_size; i++) {
        snprintf(cmd, sizeof(cmd), "SET BatchKey%d_%d Value%d_%d", i, n, i, n);
        send_msg(connfd, cmd, strlen(cmd));
        // printf("cmd: %s\n", cmd);
        memset(cmd, 0, sizeof(cmd));

        usleep(50000);  // 参数单位是微秒  不在出现粘包
        memcpy(expected + expected_len, "OK\r\n", 4);
        expected_len += 4;
    }

    char resp[1024] = {0};
    int total = 0;
    while (total < expected_len) {
        int ret = recv_msg(connfd, resp + total, sizeof(resp) - total);
        if (ret == 0) {
            break;
        }
        total += ret;
    }

    if (total != expected_len || memcmp(resp, expected, expected_len) != 0) {
        printf("--> NO PASS --> testcase_batch (expected %d bytes, got %d)\n", expected_len, total);
        printf("recv head: %.32s\n", resp);
        printf("expect head: %.32s\n", expected);
        exit(1);
    }

    printf("--> PASS --> testcase_batch, recv %d bytes\n", total);
    return 0;
}

int hash_batch_testcase(int connfd){
    int count = 10;

	struct timeval tv_begin, tv_end;
	gettimeofday(&tv_begin, NULL);

    for(int i =0; i < count; i++){
        batch_testcase(connfd, i);
        usleep(500);  // 参数单位是微秒
    }

	gettimeofday(&tv_end, NULL);
    int used_time = TIME_SUB_MS(tv_end, tv_begin);

    int qps = 0;
    if (used_time > 0) { qps = count * 10 * 1000 / used_time;} // Floating point exception (core dumped) 除零错误
    printf("hash_batch_testcase used_time: %d, qps: %d\n", used_time, qps);

    return used_time;
}

void array_testcase(int connfd){

    testcase(connfd, "SET Teacher King", "OK\r\n", "SET-Teacher");
    testcase(connfd, "GET Teacher", "King\r\n", "GET-Teacher");
    testcase(connfd, "MOD Teacher Nie", "OK\r\n", "MOD-Teacher");
    testcase(connfd, "GET Teacher", "Nie\r\n", "GET-Teacher");
    testcase(connfd, "EXIST Teacher", "EXIST\r\n", "GET-Teacher");
    testcase(connfd, "DEL Teacher", "OK\r\n", "DEL-Teacher");
    testcase(connfd, "GET Teacher", "NO EXIST\r\n", "GET-Teacher");
    testcase(connfd, "MOD Teacher KING", "NO EXIST\r\n", "MOD-Teacher");
    testcase(connfd, "MOD Teacher Nie", "NO EXIST\r\n", "MOD-Teacher");
    testcase(connfd, "EXIST Teacher", "NO EXIST\r\n", "EXIST-Teacher");
}

void rbtree_testcase(int connfd){

	testcase(connfd, "RSET Teacher King", "OK\r\n", "RSET-Teacher");
	testcase(connfd, "RGET Teacher", "King\r\n", "RGET-Teacher");
	testcase(connfd, "RMOD Teacher NIE", "OK\r\n", "RMOD-Teacher");
	testcase(connfd, "RGET Teacher", "NIE\r\n", "RGET-Teacher");
	testcase(connfd, "REXIST Teacher", "REXIST\r\n", "REXIST-Teacher");
	testcase(connfd, "RDEL Teacher", "OK\r\n", "RDEL-Teacher");
	testcase(connfd, "RGET Teacher", "NO EXIST\r\n", "RGET-Teacher");
	testcase(connfd, "RMOD Teacher KING", "NO EXIST\r\n", "RMOD-Teacher");
    testcase(connfd, "RMOD Teacher NIE", "NO EXIST\r\n", "RMOD-Teacher");
	testcase(connfd, "REXIST Teacher", "NO EXIST\r\n", "REXIST-Teacher");
}

void hash_testcase(int connfd){

	testcase(connfd, "HSET Teacher King", "OK\r\n", "HSET-Teacher");
	testcase(connfd, "HGET Teacher", "King\r\n", "HGET-Teacher");
	testcase(connfd, "HMOD Teacher NIE", "OK\r\n", "HMOD-Teacher");
	testcase(connfd, "HGET Teacher", "NIE\r\n", "HGET-Teacher");
	testcase(connfd, "HEXIST Teacher", "HEXIST\r\n", "HEXIST-Teacher");
	testcase(connfd, "HDEL Teacher", "OK\r\n", "HDEL-Teacher");
	testcase(connfd, "HGET Teacher", "NO EXIST\r\n", "HGET-Teacher");
	testcase(connfd, "HMOD Teacher KING", "NO EXIST\r\n", "HMOD-Teacher");
    testcase(connfd, "HMOD Teacher NIE", "NO EXIST\r\n", "HMOD-Teacher");
	testcase(connfd, "HEXIST Teacher", "NO EXIST\r\n", "HEXIST-Teacher");
}

void skiplist_testcase(int connfd){

	testcase(connfd, "LSET Teacher King", "OK\r\n", "LSET-Teacher");
	testcase(connfd, "LGET Teacher", "King\r\n", "LGET-Teacher");
	testcase(connfd, "LMOD Teacher NIE", "OK\r\n", "LMOD-Teacher");
	testcase(connfd, "LGET Teacher", "NIE\r\n", "LGET-Teacher");
	testcase(connfd, "LEXIST Teacher", "LEXIST\r\n", "LEXIST-Teacher");
	testcase(connfd, "LDEL Teacher", "OK\r\n", "LDEL-Teacher");
	testcase(connfd, "LGET Teacher", "NO EXIST\r\n", "LGET-Teacher");
	testcase(connfd, "LMOD Teacher KING", "NO EXIST\r\n", "LMOD-Teacher");
    testcase(connfd, "LMOD Teacher NIE", "NO EXIST\r\n", "LMOD-Teacher");
	testcase(connfd, "LEXIST Teacher", "NO EXIST\r\n", "LEXIST-Teacher");
}

int array_testcase_1w(int connfd){
    int count = 10000;

	struct timeval tv_begin, tv_end;
	gettimeofday(&tv_begin, NULL);

    for(int i =0; i < count; i++){
        array_testcase(connfd);
    }

	gettimeofday(&tv_end, NULL);
    int used_time = TIME_SUB_MS(tv_end, tv_begin);

    int qps = 0;
    if (used_time > 0) { qps = count * 10 * 1000 / used_time;} // Floating point exception (core dumped) 除零错误
    printf("array_testcase_1w used_time: %d, qps: %d\n", used_time, qps);

    return used_time;
}



int hash_testcase_1w(int connfd){
    int count = 10000;

	struct timeval tv_begin, tv_end;
	gettimeofday(&tv_begin, NULL);

    for(int i =0; i < count; i++){
        hash_testcase(connfd);
    }

	gettimeofday(&tv_end, NULL);
    int used_time = TIME_SUB_MS(tv_end, tv_begin);

    int qps = 0;
    if (used_time > 0) { qps = count * 10 * 1000 / used_time;} // Floating point exception (core dumped) 除零错误
    printf("hash_testcase_1w used_time: %d, qps: %d\n", used_time, qps);

    return used_time;
}

int rbtree_testcase_1w(int connfd){
    int count = 10000;

	struct timeval tv_begin, tv_end;
	gettimeofday(&tv_begin, NULL);

    for(int i =0; i < count; i++){
        rbtree_testcase(connfd);
    }

	gettimeofday(&tv_end, NULL);
    int used_time = TIME_SUB_MS(tv_end, tv_begin);

    int qps = 0;
    if (used_time > 0) { qps = count * 10 * 1000 / used_time;} // Floating point exception (core dumped) 除零错误
    printf("rbtree_testcase_1w used_time: %d, qps: %d\n", used_time, qps);

    return used_time;
}


int skiplist_testcase_1w(int connfd){
    int count = 10000;

	struct timeval tv_begin, tv_end;
	gettimeofday(&tv_begin, NULL);

    for(int i =0; i < count; i++){
        skiplist_testcase(connfd);
    }

	gettimeofday(&tv_end, NULL);
    int used_time = TIME_SUB_MS(tv_end, tv_begin);

    int qps = 0;
    if (used_time > 0) { qps = count * 10 * 1000 / used_time;} // Floating point exception (core dumped) 除零错误
    printf("skiplist_testcase_1w used_time: %d, qps: %d\n", used_time, qps);

    return used_time;
}

int array_testcase_2w(int connfd){
    int count = 1000;

    struct timeval tv_begin, tv_end;
    gettimeofday(&tv_begin, NULL);

    for(int i = 0; i < count; i++) {

        char cmd[128] = {0};
        printf("SET Teacher%d King%d\n", i, i);
        snprintf(cmd, 128, "SET Teacher%d King%d", i, i);
        testcase(connfd, cmd, "OK\r\n", "SET-Teacher");
    }

    for(int i = 0; i < count; i++) {

        char cmd[128] = {0};
        snprintf(cmd, 128, "GET Teacher%d", i);
        char result[128] = {0};
        snprintf(result, 128, "King%d\r\n", i);
        testcase(connfd, cmd, result, "GET-Teacher");
    }

    for(int i = 0; i < count; i++) {
        
        char cmd[128] = {0};
        snprintf(cmd, 128, "MOD Teacher%d Nie%d", i, i);
        testcase(connfd, cmd, "OK\r\n", "GET-Teacher");
    }

    // for(int i = 0; i < count; i++){
    //     char cmd[128] = {0};
    //     snprintf(cmd, 128, "RDEL Teacher%d", i);
    //     testcase(connfd, cmd, "OK\r\n", "RDEL-Teacher");
    // }

    gettimeofday(&tv_end, NULL);
    int used_time = TIME_SUB_MS(tv_end, tv_begin);
    int qps = 0;
    if (used_time > 0) { qps = 3 * count * 1000 / used_time;} // Floating point exception (core dumped) 除零错误
    
    printf("rbtree_testcase_3w used_time: %d, qps: %d\n", used_time, qps);
    return 0;
}

int hash_testcase_100w(int connfd){
    int count = 250000;

    struct timeval tv_begin, tv_end;
    gettimeofday(&tv_begin, NULL);

    for(int i = 0; i < count; i++) {

        char cmd[128] = {0};
        // printf("HSET Teacher%d King%d\n", i, i);
        snprintf(cmd, 128, "HSET Teacher%d King%d", i, i);
        testcase(connfd, cmd, "OK\r\n", "HSET-Teacher");
    }
    for(int i = 0; i < count; i++) {
        
        char cmd[128] = {0};
        snprintf(cmd, 128, "HMOD Teacher%d Nie%d", i, i);
        testcase(connfd, cmd, "OK\r\n", "HMOD-Teacher");
    }
    for(int i = 0; i < count; i++) {

        char cmd[128] = {0};
        snprintf(cmd, 128, "HGET Teacher%d", i);
        char result[128] = {0};
        snprintf(result, 128, "Nie%d\r\n", i);
        testcase(connfd, cmd, result, "HGET-Teacher");
    }
    for(int i = 0; i < count; i++){
        char cmd[128] = {0};
        snprintf(cmd, 128, "HDEL Teacher%d", i);
        testcase(connfd, cmd, "OK\r\n", "HDEL-Teacher");
    }

    gettimeofday(&tv_end, NULL);
    int used_time = TIME_SUB_MS(tv_end, tv_begin);
    int qps = 0;
    if (used_time > 0) { qps = 3 * count * 1000 / used_time;} // Floating point exception (core dumped) 除零错误
    
    printf("hash_testcase_100w used_time: %d, qps: %d\n", used_time, qps);
    return 0;
}


int hash_testcase_100w_pro(int connfd){
    int count = 250000;
    const int DEL_LATENCY = 1000; // 延迟删除，以避免立即删除刚插入的键

    struct timeval tv_begin, tv_end;
    gettimeofday(&tv_begin, NULL);

    int set_idx = 0;

    for(int i = 0; i < count; i++) {
        char cmd[128] = {0};
        snprintf(cmd, 128, "HSET Teacher%d King%d", set_idx, set_idx);
        testcase(connfd, cmd, "OK\r\n", "HSET-Teacher");

        // 当插入超过延迟后，开始删除早期的键，形成交替但有延迟的 set/del 模式
        if (set_idx >= DEL_LATENCY) {
            int del_idx = set_idx - DEL_LATENCY;
            memset(cmd, 0, sizeof(cmd));
            snprintf(cmd, 128, "HDEL Teacher%d", del_idx);
            testcase(connfd, cmd, "OK\r\n", "HDEL-Teacher");
        }

        set_idx++;
    }

    // 清理剩余未删除的键
    for (int i = set_idx - DEL_LATENCY; i < set_idx; i++){
        if (i < 0) continue;
        char cmd[128] = {0};
        snprintf(cmd, 128, "HDEL Teacher%d", i);
        testcase(connfd, cmd, "OK\r\n", "HDEL-Teacher");
    }

    gettimeofday(&tv_end, NULL);
    int used_time = TIME_SUB_MS(tv_end, tv_begin);
    int qps = 0;
    if (used_time > 0) { qps = 2 * count * 1000 / used_time; }
    printf("hash_testcase_100w_pro used_time: %d, qps: %d\n", used_time, qps);

    return used_time;
}

int rbtree_testcase_100w_pro(int connfd){
    int count = 250000;
    const int DEL_LATENCY = 1000; // 延迟删除，以避免立即删除刚插入的键

    struct timeval tv_begin, tv_end;
    gettimeofday(&tv_begin, NULL);

    int set_idx = 0;

    for(int i = 0; i < count; i++) {
        char cmd[128] = {0};
        snprintf(cmd, 128, "RSET Teacher%d King%d", set_idx, set_idx);
        testcase(connfd, cmd, "OK\r\n", "RSET-Teacher");

        // 当插入超过延迟后，开始删除早期的键，形成交替但有延迟的 set/del 模式
        if (set_idx >= DEL_LATENCY) {
            int del_idx = set_idx - DEL_LATENCY;
            memset(cmd, 0, sizeof(cmd));
            snprintf(cmd, 128, "RDEL Teacher%d", del_idx);
            testcase(connfd, cmd, "OK\r\n", "RDEL-Teacher");
        }

        set_idx++;
    }

    // 清理剩余未删除的键
    for (int i = set_idx - DEL_LATENCY; i < set_idx; i++){
        if (i < 0) continue;
        char cmd[128] = {0};
        snprintf(cmd, 128, "RDEL Teacher%d", i);
        testcase(connfd, cmd, "OK\r\n", "RDEL-Teacher");
    }

    gettimeofday(&tv_end, NULL);
    int used_time = TIME_SUB_MS(tv_end, tv_begin);
    int qps = 0;
    if (used_time > 0) { qps = 2 * count * 1000 / used_time; }
    printf("rbtree_testcase_100w_pro used_time: %d, qps: %d\n", used_time, qps);

    return used_time;
}

int rbtree_testcase_3w(int connfd){
    int count = 10000;

    struct timeval tv_begin, tv_end;
    gettimeofday(&tv_begin, NULL);

    for(int i = 0; i < count; i++) {

        char cmd[128] = {0};
        snprintf(cmd, 128, "RSET Teacher%d King%d", i, i);
        testcase(connfd, cmd, "OK\r\n", "RSET-Teacher");
    }

    for(int i = 0; i < count; i++) {

        char cmd[128] = {0};
        snprintf(cmd, 128, "RGET Teacher%d", i);
        char result[128] = {0};
        snprintf(result, 128, "King%d\r\n", i);
        testcase(connfd, cmd, result, "RGET-Teacher");
    }

    for(int i = 0; i < count; i++) {
        
        char cmd[128] = {0};
        snprintf(cmd, 128, "RMOD Teacher%d Nie%d", i, i);
        testcase(connfd, cmd, "OK\r\n", "RGET-Teacher");
    }

    for(int i = 0; i < count; i++){
        char cmd[128] = {0};
        snprintf(cmd, 128, "RDEL Teacher%d", i);
        testcase(connfd, cmd, "OK\r\n", "RDEL-Teacher");
    }

    gettimeofday(&tv_end, NULL);
    int used_time = TIME_SUB_MS(tv_end, tv_begin);
    int qps = 0;
    if (used_time > 0) { qps = 3 * count * 1000 / used_time;} // Floating point exception (core dumped) 除零错误
    
    printf("rbtree_testcase_3w used_time: %d, qps: %d\n", used_time, qps);
    return 0;
}



// ./build/bin/testcase 10.66.189.100 2000 0
int main(int argc, char *argv[]) {

    if(argc != 4) {
        perror("arg error\n");
        return -1;
    }

    char *ip = argv[1];
    int port = atoi(argv[2]);
    int mode = atoi(argv[3]);

    int connfd = connect_tcpserver(ip, port);

    if(mode == 0){
        array_testcase_1w(connfd);
    }else if(mode == 1){
        rbtree_testcase_1w(connfd);
    }else if(mode == 2){
        rbtree_testcase_3w(connfd);
    }else if(mode == 3){
        hash_testcase(connfd);
    }else if(mode == 4){
        hash_testcase_1w(connfd);
    }else if(mode == 5){
        skiplist_testcase_1w(connfd);
    }else if(mode == 6){
        array_testcase_2w(connfd);
    }else if(mode == 7){
        hash_testcase_100w(connfd);
    }else if(mode == 8){
        hash_batch_testcase(connfd);
    } else if(mode == 9){
        rbtree_testcase_100w_pro(connfd);
    } else if(mode == 10){
        hash_testcase_100w_pro(connfd);
    }

    return 0;
}



