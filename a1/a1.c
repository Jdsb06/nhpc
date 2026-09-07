#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#define MAX_SIZE (4 * 1024 * 1024) // 4 MB (4194304 bytes)
#define WARMUP_ITERS 5
#define BENCH_ITERS 10

// Reliable write wrapper chunking large buffers
int send_all(int fd, const char *buf, size_t total_bytes, size_t chunk_size) {
    size_t bytes_sent = 0;
    while (bytes_sent < total_bytes) {
        size_t to_send = total_bytes - bytes_sent;
        if (to_send > chunk_size) {
            to_send = chunk_size;
        }
        ssize_t n = send(fd, buf + bytes_sent, to_send, MSG_NOSIGNAL);
        if (n <= 0) {
            return -1;
        }
        bytes_sent += (size_t)n;
    }
    return 0;
}

// Reliable read wrapper chunking large buffers
int recv_all(int fd, char *buf, size_t total_bytes, size_t chunk_size) {
    size_t bytes_received = 0;
    while (bytes_received < total_bytes) {
        size_t to_recv = total_bytes - bytes_received;
        if (to_recv > chunk_size) {
            to_recv = chunk_size;
        }
        ssize_t n = read(fd, buf + bytes_received, to_recv);
        if (n <= 0) {
            return -1;
        }
        bytes_received += (size_t)n;
    }
    return 0;
}

void run_server(int port, size_t chunk_size) {
    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) { perror("socket"); exit(1); }

    int opt = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(sfd);
        exit(1);
    }
    if (listen(sfd, 1) < 0) {
        perror("listen");
        close(sfd);
        exit(1);
    }

    printf("Server listening on port %d (Chunk Size: %zu bytes)...\n", port, chunk_size);

    char *buf = (char *)malloc(MAX_SIZE);
    if (!buf) { perror("malloc"); close(sfd); exit(1); }

    while (1) {
        struct sockaddr_in cli;
        socklen_t len = sizeof(cli);
        int cfd = accept(sfd, (struct sockaddr *)&cli, &len);
        if (cfd < 0) { perror("accept"); continue; }

        int nodelay = 1;
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, (char *)&nodelay, sizeof(nodelay));

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &cli.sin_addr, ip, sizeof(ip));
        printf("Client connected: %s:%d\n", ip, ntohs(cli.sin_port));

        int client_ok = 1;
        for (size_t size = 1; size <= MAX_SIZE; size *= 2) {
            // Warmup iterations for current message size
            for (int i = 0; i < WARMUP_ITERS; i++) {
                if (recv_all(cfd, buf, size, chunk_size) != 0 ||
                    send_all(cfd, buf, size, chunk_size) != 0) {
                    client_ok = 0;
                    break;
                }
            }
            if (!client_ok) break;

            // Measured benchmark iterations
            for (int i = 0; i < BENCH_ITERS; i++) {
                if (recv_all(cfd, buf, size, chunk_size) != 0 ||
                    send_all(cfd, buf, size, chunk_size) != 0) {
                    client_ok = 0;
                    break;
                }
            }
            if (!client_ok) break;
        }

        if (client_ok) {
            printf("Benchmark completed for client.\n");
        } else {
            printf("Client disconnected unexpectedly.\n");
        }
        close(cfd);
    }

    free(buf);
    close(sfd);
}

void run_client(const char *host, int port, size_t chunk_size) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); exit(1); }

    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *)&nodelay, sizeof(nodelay));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        fprintf(stderr, "Invalid address: %s\n", host);
        close(fd);
        exit(1);
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        exit(1);
    }

    char *send_buf = (char *)malloc(MAX_SIZE);
    char *recv_buf = (char *)malloc(MAX_SIZE);
    if (!send_buf || !recv_buf) {
        perror("malloc");
        free(send_buf);
        free(recv_buf);
        close(fd);
        exit(1);
    }
    memset(send_buf, 'X', MAX_SIZE);

    printf("%-14s %-20s %-16s\n", "Msg size", "Time (usecs)", "Bandwidth (MB/s)");

    for (size_t size = 1; size <= MAX_SIZE; size *= 2) {
        // 1. Warmup per size (Unmeasured)
        int failed = 0;
        for (int i = 0; i < WARMUP_ITERS; i++) {
            if (send_all(fd, send_buf, size, chunk_size) != 0 ||
                recv_all(fd, recv_buf, size, chunk_size) != 0) {
                failed = 1;
                break;
            }
        }
        if (failed) {
            fprintf(stderr, "Connection failed during warmup for size %zu\n", size);
            break;
        }

        // 2. Timed benchmark loop
        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);

        for (int i = 0; i < BENCH_ITERS; i++) {
            if (send_all(fd, send_buf, size, chunk_size) != 0 ||
                recv_all(fd, recv_buf, size, chunk_size) != 0) {
                failed = 1;
                break;
            }
        }
        if (failed) {
            fprintf(stderr, "Connection failed during benchmark for size %zu\n", size);
            break;
        }

        clock_gettime(CLOCK_MONOTONIC, &end);

        double total_elapsed_usec = (double)(end.tv_sec - start.tv_sec) * 1e6 +
                                    (double)(end.tv_nsec - start.tv_nsec) / 1e3;
        
        // Average Round-Trip Time per message
        double avg_rtt_usec = total_elapsed_usec / (double)BENCH_ITERS;

        // Bandwidth: total bidirectional bytes transferred (2 * size) over elapsed time
        double total_bytes = (double)(2 * size) * BENCH_ITERS;
        double bandwidth_MBs = (total_bytes / (1024.0 * 1024.0)) / (total_elapsed_usec / 1e6);

        printf("%-14zu %-20.2f %-16.2f\n", size, avg_rtt_usec, bandwidth_MBs);
    }

    free(send_buf);
    free(recv_buf);
    close(fd);
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);

    if (argc < 2) {
        printf("Usage:\n");
        printf("  Server: %s server [port]\n", argv[0]);
        printf("  Client: %s client <server_ip> [port]\n", argv[0]);
        return 1;
    }

    char *chunk_env = getenv("CHUNK_SIZE");
    size_t chunk_size = 65536; // 64 KB default
    if (chunk_env != NULL) {
        int parsed_chunk = atoi(chunk_env);
        if (parsed_chunk > 0) {
            chunk_size = (size_t)parsed_chunk;
        }
    }

    if (strcmp(argv[1], "server") == 0) {
        int port = (argc > 2) ? atoi(argv[2]) : 6969;
        run_server(port, chunk_size);
    } else if (strcmp(argv[1], "client") == 0) {
        if (argc < 3) {
            printf("Client requires target IP address: %s client <server_ip> [port]\n", argv[0]);
            return 1;
        }
        const char *host = argv[2];
        int port = (argc > 3) ? atoi(argv[3]) : 6969;
        run_client(host, port, chunk_size);
    } else {
        printf("Invalid mode. Use 'server' or 'client'.\n");
        return 1;
    }

    return 0;
}