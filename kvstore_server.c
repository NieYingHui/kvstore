#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kvstore.h"
#include "src/kvs_config.h"
#include "src/engine.h"

static void print_usage(const char *prog) {
    printf("Usage:\n");
    printf("  %s [kvstore.conf] [--port <port>]\n", prog);
    printf("Options:\n");
    printf("  --port <port>     Override port defined in config (default 2000)\n");
    printf("  -h, --help        Show this help and exit\n");
    printf("Examples:\n");
    printf("  %s kvstore.conf\n", prog);
    printf("  %s --port 2000\n", prog);
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

int main(int argc, char *argv[]) {
    kvs_server_config_t cfg;
    kvs_server_config_init(&cfg);

    const char *conf_file = NULL;
    char errbuf[256] = {0};

    int i = 1;
    while (i < argc) {
        const char *arg = argv[i];
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(arg, "--port") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value for --port\n");
                return 1;
            }
            unsigned short p = 0;
            if (parse_port(argv[i + 1], &p) != 0) {
                fprintf(stderr, "Invalid port: %s\n", argv[i + 1]);
                return 1;
            }
            cfg.port = p;
            i += 2;
        } else if (arg[0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", arg);
            print_usage(argv[0]);
            return 1;
        } else {

            int all_digits = 1;
            for (const char *p = arg; *p; ++p) {
                if (*p < '0' || *p > '9') {
                    all_digits = 0;
                    break;
                }
            }
            if (all_digits) {
                unsigned short p = 0;
                if (parse_port(arg, &p) != 0) {
                    fprintf(stderr, "Invalid port: %s\n", arg);
                    return 1;
                }
                cfg.port = p;
            } else {
                conf_file = arg;
            }
            i++;
        }
    }

    if (conf_file) {
        if (kvs_load_config(conf_file, &cfg, errbuf, sizeof(errbuf)) != 0) {
            fprintf(stderr, "Failed to load config '%s': %s\n",
                    conf_file,
                    errbuf[0] ? errbuf : "unknown error");
            return 1;
        }
    }

    /* 将最终配置写入全局服务器状态，便于其他模块统一访问。 */
    kvs_server.config = cfg;
    kvs_server.port   = cfg.port;

    printf("kvstore-server starting on port %u\n", cfg.port);
    return kvs_server_run(cfg.port);
}
