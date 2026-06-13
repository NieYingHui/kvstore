#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <errno.h>

/* Set KVSCLI_RESP_DEBUG=1 to print RESP parsing details to stderr. */
static int g_resp_debug = 0;
static int g_resp_raw = 0;
static int g_quiet = 0;

#define KVSCLI_DEFAULT_HOST "127.0.0.1"
#define KVSCLI_DEFAULT_PORT 2000
#define KVSCLI_BUF_SIZE 1024

#define KVSCLI_RESP_MAX_ARGS 256

static int send_all(int sockfd, const void *data, size_t len) {
    const char *p = (const char *)data;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(sockfd, p + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("send");
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

// 确保从套接字(sockfd)中精确接收指定长度(len)的数据
static int recv_exact(int sockfd, void *out, size_t len) {
    char *p = (char *)out;
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(sockfd, p + got, len - got, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("recv");
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        got += (size_t)n;
    }
    return 0;
}

// 从套接字(socket)中接收一行以CRLF（回车+换行）结尾的数据
static int recv_line_crlf(int sockfd, char *buf, size_t cap, size_t *out_len) {
    if (!buf || cap < 3) return -1;
    size_t pos = 0;
    while (1) {
        char ch;
        ssize_t n = recv(sockfd, &ch, 1, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("recv");
            return -1;
        }
        if (n == 0) return -1;
        if (pos + 1 >= cap) {
            return -1;
        }
        buf[pos++] = ch;
        if (pos >= 2 && buf[pos - 2] == '\r' && buf[pos - 1] == '\n') {
            buf[pos] = '\0';
            if (out_len) *out_len = pos;
            return 0;
        }
    }
}

static void print_indent(int depth) { // 根据指定的深度缩进打印空格
    for (int i = 0; i < depth; i++) {
        fwrite("  ", 1, 2, stdout);
    }
}

// 解析和打印Redis序列化协议(RESP)回复的递归函数
static int resp_print_reply_recursive(int sockfd, int depth, int index_in_array) {
    unsigned char type;
    if (recv_exact(sockfd, &type, 1) != 0) {
        fprintf(stderr, "Failed to read RESP type\n");
        return -1;
    }

    char line[256];
    size_t line_len = 0;

    if (type == '+' || type == '-' || type == ':') { // 简单字符串('+')、错误('-')、整数(':'): 直接读取并打印内容
        if (recv_line_crlf(sockfd, line, sizeof(line), &line_len) != 0) {
            fprintf(stderr, "Failed to read RESP line\n");
            return -1;
        }

        if (!g_quiet) {
            if (index_in_array > 0) {
                print_indent(depth);
                printf("%d) ", index_in_array);
            } else if (depth > 0) {
                print_indent(depth);
            }

            /* 不带尾随换行符打印 */
            if (line_len >= 2) {
                fwrite(line, 1, line_len - 2, stdout);
            }
            fwrite("\n", 1, 1, stdout);
        }
        return (type == '-') ? -1 : 0;
    }

    if (type == '$') { // 块字符串('$'): 读取长度，然后读取指定长度的数据 $6\r\nfoobar\r\n
        if (recv_line_crlf(sockfd, line, sizeof(line), &line_len) != 0) {
            fprintf(stderr, "Failed to read bulk length\n");
            return -1;
        }
        if (g_resp_debug) {
            /* line includes trailing CRLF; print without it */
            size_t n = (line_len >= 2) ? (line_len - 2) : 0;
            fprintf(stderr, "[resp] bulk len line: '%.*s'\n", (int)n, line);
        }
        long blen = strtol(line, NULL, 10); // 长度为10进制
        if (g_resp_debug) {
            fprintf(stderr, "[resp] bulk len parsed: %ld\n", blen);
        }
        if (blen == -1) {
            if (!g_quiet) {
                if (index_in_array > 0) {
                    print_indent(depth);
                    printf("%d) ", index_in_array);
                } else if (depth > 0) {
                    print_indent(depth);
                }
                fwrite("(nil)\n", 1, 6, stdout);
            }
            return 0;
        }
        if (blen < 0) {
            fprintf(stderr, "Invalid bulk length\n");
            return -1;
        }

        char *data = (char *)malloc((size_t)blen);
        if (!data && blen > 0) {
            fprintf(stderr, "Out of memory\n");
            return -1;
        }
        if (blen > 0 && recv_exact(sockfd, data, (size_t)blen) != 0) { // 调用recv_exact读取数据
            free(data);
            fprintf(stderr, "Failed to read bulk payload\n");
            return -1;
        }
        char crlf[2];
        if (recv_exact(sockfd, crlf, 2) != 0 || crlf[0] != '\r' || crlf[1] != '\n') { // // 调用recv_exact读取数据\r\n
            free(data);
            fprintf(stderr, "Invalid bulk terminator\n");
            return -1;
        }

        if (!g_quiet) {
            if (index_in_array > 0) {
                print_indent(depth);
                printf("%d) ", index_in_array);
            } else if (depth > 0) {
                print_indent(depth);
            }

            if (blen > 0) {
                fwrite(data, 1, (size_t)blen, stdout);
            }
        }
        free(data);

        /* 默认模式保持与 redis-cli 类似的行为；原始模式精确保留字节。 */
        if (!g_quiet && !g_resp_raw) {
            fwrite("\n", 1, 1, stdout);
        }
        return 0;
    }

    if (type == '*') { // 数组('*'): 读取数组长度，然后递归处理每个元素 *3\r\n:1\r\n:2\r\n:3\r\n
        if (recv_line_crlf(sockfd, line, sizeof(line), &line_len) != 0) {
            fprintf(stderr, "Failed to read array length\n");
            return -1;
        }
        long n = strtol(line, NULL, 10); // 长度为10进制
        if (n == -1) {
            if (!g_quiet) {
                if (index_in_array > 0) {
                    print_indent(depth);
                    printf("%d) ", index_in_array);
                } else if (depth > 0) {
                    print_indent(depth);
                }
                fwrite("(nil)\n", 1, 6, stdout);
            }
            return 0;
        }
        if (n < 0) {
            fprintf(stderr, "Invalid array length\n");
            return -1;
        }

        /* 对于嵌套数组，打印类似 redis-cli 的标题行。 */
        if (!g_quiet && index_in_array > 0) {
            print_indent(depth);
            printf("%d) (array %ld)\n", index_in_array, n);
        }
        for (long i = 0; i < n; i++) {
            int child_index = (depth == 0) ? (int)(i + 1) : (int)(i + 1);
            if (resp_print_reply_recursive(sockfd, depth + 1, child_index) != 0) {
                return -1;
            }
        }
        return 0;
    }

    fprintf(stderr, "Unsupported RESP reply type: %c\n", (char)type);
    return -1;
}

// 打印解析resp
static int resp_print_reply(int sockfd) {
    return resp_print_reply_recursive(sockfd, 0, 0);
}

// 将命令行参数转换为Redis的RESP协议格式
static int resp_build_command(int argc, char *argv[], char **out, size_t *out_len) {
// char *cmd;
// size_t len;
// const char *args[] = {"SET", "key", "value"};
// int result = resp_build_command(3, (char **)args, &cmd, &len);

    if (!out || !out_len) return -1;
    *out = NULL;
    *out_len = 0;
    if (argc <= 0) return -1;
    if (argc > KVSCLI_RESP_MAX_ARGS) {
        fprintf(stderr, "Too many args (max %d)\n", KVSCLI_RESP_MAX_ARGS);
        return -1;
    }

    size_t needed = 64;
    for (int i = 0; i < argc; ++i) {
        needed += strlen(argv[i]) + 64;
    }
    char *buf = (char *)malloc(needed);
    if (!buf) return -1;

    size_t pos = 0;
    int n = snprintf(buf + pos, needed - pos, "*%d\r\n", argc); // buf变为"*3\r\n"
    if (n < 0) { free(buf); return -1; }
    pos += (size_t)n;
    for (int i = 0; i < argc; ++i) { // 处理每一个参数
        size_t alen = strlen(argv[i]);
        n = snprintf(buf + pos, needed - pos, "$%zu\r\n", alen);
        if (n < 0) { free(buf); return -1; }
        pos += (size_t)n;
        if (pos + alen + 2 > needed) { free(buf); return -1; }
        memcpy(buf + pos, argv[i], alen);
        pos += alen;
        buf[pos++] = '\r';
        buf[pos++] = '\n';
    }

    *out = buf; // *3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n
    *out_len = pos; // 39
    return 0;
}
// 通过套接字(sockfd)发送一个RESP(REdis Serialization Protocol)命令并打印服务器的回复
static int resp_send_command(int sockfd, int argc, char *argv[]) {
    char *frame = NULL;
    size_t frame_len = 0;
    if (resp_build_command(argc, argv, &frame, &frame_len) != 0) {
        fprintf(stderr, "Failed to build RESP command\n");
        return -1;
    }
    int rc = 0;
    if (send_all(sockfd, frame, frame_len) != 0) {
        free(frame);
        return -1;
    }
    free(frame);

    rc = resp_print_reply(sockfd); // 打印服务器的回复
    return rc;
}

static void print_usage(const char *prog) {
    printf("Usage:\n");
    printf("  %s [-h host] [-p port]                   # interactive mode\n", prog);
    printf("  %s [-h host] [-p port] cmd [arg..]        # single command mode\n", prog);
    printf("  %s --resp [-h host] [-p port] cmd [arg..] # RESP mode (recommended for large/multiline values)\n", prog);
    printf("  %s --pipe [--resp] [-h host] [-p port] [--pipe-from <file>]\n", prog);
    printf("Options:\n");
    printf("  -h <host>       Server hostname or IP (default 127.0.0.1)\n");
    printf("  -p <port>       Server port (default 2000)\n");
    printf("  --resp          Use RESP protocol for request/reply\n");
    printf("  --resp-raw      Use RESP and print bulk without extra newline\n");
    printf("  --pipe          Read commands from stdin (or --pipe-from) and stream to server\n");
    printf("  --pipe-from <f> Read commands from file instead of stdin\n");
    printf("  --timeout-ms <n> Set socket send/recv timeout (milliseconds)\n");
    printf("  --quiet         Do not print replies (useful for pipe mode)\n");
    printf("  --help          Show this help and exit\n");
    printf("Notes:\n");
    printf("  - In text mode, pipe input should be one command per line (\\n delimited).\n");
    printf("  - In RESP+pipe mode, stdin/file should contain raw RESP frames (like redis-cli --pipe).\n");
}

static int parse_port(const char *s, unsigned short *out) {
    if (!s || !out) return -1;
    char *endptr = NULL;
    long v = strtol(s, &endptr, 10);
    if (endptr == s || *endptr != '\0' || v <= 0 || v > 65535) {
        return -1;
    }
    *out = (unsigned short)v;
    return 0;
}

static int connect_tcp (const char *host, unsigned short port) {
    struct sockaddr_in addr;
    int sock = socket(AF_INET, SOCK_STREAM, 0);

    if(sock < 0) return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if(inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        close(sock);
        return -1;
    }

    if(connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0){
        close(sock);
        return -1;
    }

    return sock;
}

static int set_socket_timeout_ms(int sockfd, int timeout_ms) {
    if (timeout_ms <= 0) return 0;
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) { // 接收超时(SO_RCVTIMEO)
        perror("setsockopt SO_RCVTIMEO");
        return -1;
    }
    if (setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) { // 发送超时(SO_SNDTIMEO)
        perror("setsockopt SO_SNDTIMEO");
        return -1;
    }
    return 0;
}

// 通过网络套接字发送命令并接收响应的函数
static int send_command(int sockfd, const char *cmd) {
    size_t len = strlen(cmd);
    if (len == 0) return 0;

    ssize_t n = send(sockfd, cmd, len, 0);
    if (n < 0) {
        perror("send");
        return -1;
    }

    char buf[KVSCLI_BUF_SIZE + 1];
    memset(buf, 0, sizeof(buf));
    n = recv(sockfd, buf, KVSCLI_BUF_SIZE, 0);
    if (n < 0) {
        perror("recv");
        return -1;
    } else if (n == 0) {
        printf("(server closed connection)\n");
        return -1;
    }

    buf[n] = '\0';
    printf("%s", buf);
    if (buf[n - 1] != '\n') {
        printf("\n");
    }
    return 0;
}

// 将一行字符串分割成多个参数
static int split_args(char *line, char *argv_out[], int max_args) {
// char line[] = "SET key value";
// char *argv[10];
// int argc = split_args(line, argv, 10);

    if (!line || !argv_out || max_args <= 0) return 0;
    int argc = 0;
    char *p = line;

    while (*p != '\0') {
        /* 跳过空格 */
        while (*p == ' ') p++;
        if (*p == '\0') break;
        if (argc >= max_args) break;
        argv_out[argc++] = p;
        /* 跳转到下一个空格 */
        while (*p != '\0' && *p != ' ') p++;
        if (*p == '\0') break;
        *p = '\0';
        p++;
    }
// argv[0]指向"SET"
// argv[1]指向"key"
// argv[2]指向"value"
    return argc;
}


// 将一行字符串按空格分割成多个参数，支持引号和转义字符
static int parse_args_quoted(char *line, char *argv_out[], int max_args) {
    if (!line || !argv_out || max_args <= 0) return 0;
    int argc = 0;
    char *p = line;

    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;
        if (argc >= max_args) break;

        char *dst = p;
        argv_out[argc++] = dst;

        int in_quotes = 0;
        if (*p == '"') {
            in_quotes = 1;
            p++;
        }

        while (*p) {
            char ch = *p++;
            if (in_quotes) {
                if (ch == '"') break;
                if (ch == '\\' && *p) {
                    char esc = *p++;
                    switch (esc) {
                    case 'n': ch = '\n'; break;
                    case 'r': ch = '\r'; break;
                    case 't': ch = '\t'; break;
                    case '\\': ch = '\\'; break;
                    case '"': ch = '"'; break;
                    default: ch = esc; break;
                    }
                }
                *dst++ = ch;
            } else {
                if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
                    break;
                }
                if (ch == '\\' && *p) {
                    /* allow escaping spaces in non-quoted mode */
                    char esc = *p++;
                    *dst++ = esc;
                } else {
                    *dst++ = ch;
                }
            }
        }
        *dst = '\0';

        while (*p == ' ' || *p == '\t') p++;
    }
    return argc;
}

static long long now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000LL + (long long)(tv.tv_usec / 1000);
}

// 将输入流(文件)的内容通过socket发送出去，然后接收并处理服务器的响应，同时提供性能统计信息。
static int pipe_stream(int sockfd, FILE *in, int use_resp) {
    char buf[64 * 1024]; // 发送缓冲区buf
    size_t nread;
    long long start = now_ms();
    long long bytes_sent = 0;
    long long cmd_count = 0;
    int at_line_start = 1;
    int saw_any = 0;
    unsigned char last_byte = 0;

    while ((nread = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (send_all(sockfd, buf, nread) != 0) {
            return -1;
        }
        bytes_sent += (long long)nread;
        saw_any = 1;
        last_byte = (unsigned char)buf[nread - 1];

        /* 仅用于统计的最佳努力命令计数。 */
        for (size_t i = 0; i < nread; i++) {
            unsigned char ch = (unsigned char)buf[i];
            if (use_resp) {
                if (at_line_start) {
                    if (ch == '*') cmd_count++;
                    at_line_start = 0;
                }
                if (ch == '\n') at_line_start = 1;
            } else {
                if (ch == '\n') cmd_count++;
            }
        }
    }


    if (!use_resp && saw_any && last_byte != '\n') {
        static const char nl = '\n';
        if (send_all(sockfd, &nl, 1) != 0) {
            return -1;
        }
        bytes_sent += 1;
        cmd_count += 1;
    }

    /* 告知服务器不再输入；然后读取回复直至文件结束。 */
    shutdown(sockfd, SHUT_WR);

    char rbuf[64 * 1024];
    for (;;) {
        ssize_t n = recv(sockfd, rbuf, sizeof(rbuf), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("recv");
            break;
        }
        if (n == 0) break;
        if (!g_quiet) {
            fwrite(rbuf, 1, (size_t)n, stdout);
        }
    }

    long long end = now_ms();
    long long dur = end - start;
    if (dur <= 0) dur = 1;
    double mbps = (double)bytes_sent / (1024.0 * 1024.0) * 1000.0 / (double)dur; // 传输速度
    double cps = (double)cmd_count * 1000.0 / (double)dur; // 命令速率
    fprintf(stderr, "[pipe] sent_bytes=%lld, approx_cmds=%lld, time_ms=%lld, MB/s=%.2f, cmds/s=%.2f\n",
            bytes_sent, cmd_count, (end - start), mbps, cps);
    return 0;
}

int main(int argc, char *argv[]) {
    const char *host = KVSCLI_DEFAULT_HOST;
    unsigned short port = KVSCLI_DEFAULT_PORT;

    int use_resp = 0;
    int pipe_mode = 0;
    const char *pipe_from = NULL;
    int timeout_ms = 0;

    if (getenv("KVSCLI_RESP_DEBUG") != NULL) {
        g_resp_debug = 1;
    }

    int i = 1;
    int first_cmd = -1;

    while (i < argc) {
        const char *arg = argv[i];
        if (strcmp(arg, "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(arg, "--quiet") == 0) {
            g_quiet = 1;
            i += 1;
        } else if (strcmp(arg, "--resp") == 0) {
            use_resp = 1;
            i += 1;
        } else if (strcmp(arg, "--resp-raw") == 0) {
            use_resp = 1;
            g_resp_raw = 1;
            i += 1;
        } else if (strcmp(arg, "--pipe") == 0) {
            pipe_mode = 1;
            i += 1;
        } else if (strcmp(arg, "--pipe-from") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value for --pipe-from\n");
                return 1;
            }
            pipe_from = argv[i + 1];
            i += 2;
        } else if (strcmp(arg, "--timeout-ms") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value for --timeout-ms\n");
                return 1;
            }
            timeout_ms = atoi(argv[i + 1]);
            if (timeout_ms < 0) timeout_ms = 0;
            i += 2;
        } else if (strcmp(arg, "-h") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value for -h\n");
                return 1;
            }
            host = argv[i + 1];
            i += 2;
        } else if (strcmp(arg, "-p") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value for -p\n");
                return 1;
            }
            unsigned short p = 0;
            if (parse_port(argv[i + 1], &p) != 0) {
                fprintf(stderr, "Invalid port: %s\n", argv[i + 1]);
                return 1;
            }
            port = p;
            i += 2;
        } else {
            first_cmd = i;
            break;
        }
    }

    int sockfd = connect_tcp(host, port);
    if (sockfd < 0) {
        return 1;
    }

    if (set_socket_timeout_ms(sockfd, timeout_ms) != 0) {
        close(sockfd);
        return 1;
    }

    int rc = 0;

    if (pipe_mode) {
        if (first_cmd != -1) {
            fprintf(stderr, "--pipe cannot be used together with a single command\n");
            close(sockfd);
            return 1;
        }
        FILE *in = stdin;
        if (pipe_from) {
            in = fopen(pipe_from, "rb");
            if (!in) {
                perror("fopen --pipe-from");
                close(sockfd);
                return 1;
            }
        }
        rc = (pipe_stream(sockfd, in, use_resp) == 0) ? 0 : 1;
        if (pipe_from && in && in != stdin) fclose(in);
        close(sockfd);
        return rc;
    }

    if (first_cmd != -1) {
        if (use_resp) {
            rc = (resp_send_command(sockfd, argc - first_cmd, &argv[first_cmd]) == 0) ? 0 : 1;
        } else {
            /* 单命令模式：用空格连接argv的其余参数。 */
            char cmd[KVSCLI_BUF_SIZE] = {0};
            size_t pos = 0;
            for (int j = first_cmd; j < argc; ++j) {
                const char *part = argv[j];
                size_t plen = strlen(part);
                if (pos + plen + 1 >= sizeof(cmd)) {
                    fprintf(stderr, "Command too long\n");
                    rc = 1;
                    break;
                }
                if (j > first_cmd) {
                    cmd[pos++] = ' ';
                }
                memcpy(cmd + pos, part, plen);
                pos += plen;
            }
            if (rc == 0) {
                cmd[pos] = '\0';
                rc = (send_command(sockfd, cmd) == 0) ? 0 : 1;
            }
        }
    } else {
        /* 交互模式。 */
        printf("Connected to %s:%u, type commands (SET/GET/DEL/...) or 'quit' to exit.\n",
               host, port);
        if (use_resp) {
            printf("RESP mode enabled: large/multiline GET output is supported.\n");
            if (g_resp_raw) {
                printf("RESP raw output enabled.\n");
            }
        }
        char line[KVSCLI_BUF_SIZE];
        while (1) {
            printf("kvstore:%s:%u> ", host, port);
            fflush(stdout);

            if (!fgets(line, sizeof(line), stdin)) {
                break; /* EOF */
            }

            /* 去除尾随换行符 */
            size_t len = strlen(line);
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
                line[--len] = '\0';
            }

            if (len == 0) {
                continue;
            }

            if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0 || strcmp(line, "q") == 0 || strcmp(line, "Q") == 0) {
                break;
            }

            if (use_resp) {
                char *args[KVSCLI_RESP_MAX_ARGS] = {0};
                int n = parse_args_quoted(line, args, KVSCLI_RESP_MAX_ARGS);
                if (n <= 0) {
                    /* 退回到旧的拆分器 */
                    n = split_args(line, args, KVSCLI_RESP_MAX_ARGS);
                }
                if (n <= 0) continue;
                if (resp_send_command(sockfd, n, args) != 0) {
                    rc = 1;
                    break;
                }
            } else {
                if (send_command(sockfd, line) != 0) {
                    rc = 1;
                    break;
                }
            }
        }
    }

    close(sockfd);
    return rc;
}
