#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <signal.h>
#include <poll.h>
#include <errno.h>

/* 録音・再生の設定（SoXを使用） */
#define AUDIO_COMMAND_REC  "rec -q -t raw -b 16 -e signed-integer -c 1 -r 44100 -"
#define AUDIO_COMMAND_PLAY "play -q -t raw -b 16 -e signed-integer -c 1 -r 44100 -"
#define BUFFER_SIZE 256

void run_server(uint16_t port);
void run_client(const char *ip, uint16_t port);

int main(int argc, char **argv) {
    // クライアント切断時のSIGPIPEでプロセスが終了するのを防ぐ
    signal(SIGPIPE, SIG_IGN);

    if (argc == 2) {
        /* 引数1個: サーバーモード (ポート指定) */
        char *endptr;
        long port = strtol(argv[1], &endptr, 10);
        if (*endptr != '\0' || port < 1024 || port > 65535) {
            fprintf(stderr, "Invalid port number: %s\n", argv[1]);
            exit(1);
        }
        run_server((uint16_t)port);
    } 
    else if (argc == 3) {
        /* 引数2個: クライアントモード (IP ポート指定) */
        char *ip = argv[1];
        char *endptr;
        long port = strtol(argv[2], &endptr, 10);
        if (*endptr != '\0' || port < 1024 || port > 65535) {
            fprintf(stderr, "Invalid port number: %s\n", argv[2]);
            exit(1);
        }
        run_client(ip, (uint16_t)port);
    } 
    else {
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  Server (Sender): %s <port>\n", argv[0]);
        fprintf(stderr, "  Client (Receiver): %s <server_ip> <port>\n", argv[0]);
        exit(1);
    }

    return 0;
}

void run_server(uint16_t port) {
    int ss = socket(PF_INET, SOCK_STREAM, 0);
    if (ss == -1) { perror("socket"); exit(1); }

    int opt = 1;
    if (setsockopt(ss, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        perror("setsockopt(SO_REUSEADDR)");
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(ss, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("bind"); exit(1);
    }

    if (listen(ss, 1) == -1) { perror("listen"); exit(1); }

    printf("Server listening on port %d... Waiting for client connection.\n", port);

    struct sockaddr_in client_addr;
    socklen_t len = sizeof(client_addr);
    int s = accept(ss, (struct sockaddr *)&client_addr, &len);
    if (s == -1) { perror("accept"); exit(1); }
    printf("Client connected: %s\n", inet_ntoa(client_addr.sin_addr));

    // 低遅延設定
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
    close(ss); // 親ソケットは不要

    // 録音と再生の両方を起動
    FILE *fp_rec = popen(AUDIO_COMMAND_REC, "r");
    FILE *fp_play = popen(AUDIO_COMMAND_PLAY, "w");
    if (!fp_rec || !fp_play) { perror("popen"); close(s); exit(1); }

    int record_fd = fileno(fp_rec);
    int play_fd = fileno(fp_play);
    unsigned char buffer[BUFFER_SIZE];
    ssize_t n;
    int error_occured = 0;
    int is_muted = 0;

    struct pollfd fds[3];
    // 0: 録音パイプ（読み込み）
    fds[0].fd = record_fd;
    fds[0].events = POLLIN;
    // 1: ソケット（受信）
    fds[1].fd = s;
    fds[1].events = POLLIN;
    // 2: 標準入力（コマンド）
    fds[2].fd = STDIN_FILENO;
    fds[2].events = POLLIN;

    printf("Full-duplex communication started.\n");
    printf("'q'+Enter: quit, 'm'+Enter: toggle microphone mute.\n");

    while (!error_occured) {
        int ret = poll(fds, 3, -1);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }

        // コマンド入力のチェック
        if (fds[2].revents & POLLIN) {
            char cmd[10];
            if (fgets(cmd, sizeof(cmd), stdin)) {
                if (cmd[0] == 'q') {
                    printf("Termination command received. Closing...\n");
                    break;
                } else if (cmd[0] == 'm') {
                    is_muted = !is_muted;
                    printf(">>> Microphone Mute: %s <<<\n", is_muted ? "ON" : "OFF");
                }
            }
        }

        // 1. 自分の声を送る (録音パイプ -> ソケット)
        if (fds[0].revents & (POLLIN | POLLHUP)) {
            n = read(record_fd, buffer, sizeof(buffer));
            if (n <= 0) break;

            // ミュート時は送信データをゼロ（無音）で上書き
            if (is_muted) memset(buffer, 0, n);

            ssize_t total_sent = 0;
            while (total_sent < n) {
                ssize_t sent = write(s, buffer + total_sent, n - total_sent);
                if (sent == -1) {
                    if (errno == EINTR) continue;
                    perror("write to socket");
                    error_occured = 1;
                    break;
                }
                total_sent += sent;
            }
        }

        // 2. 相手の声を聞く (ソケット -> 再生パイプ)
        if (fds[1].revents & (POLLIN | POLLHUP)) {
            n = read(s, buffer, sizeof(buffer));
            if (n <= 0) break;

            ssize_t total_played = 0;
            while (total_played < n) {
                ssize_t played = write(play_fd, buffer + total_played, n - total_played);
                if (played == -1) {
                    if (errno == EINTR) continue;
                    perror("write to play process");
                    error_occured = 1;
                    break;
                }
                total_played += played;
            }
        }
    }

    pclose(fp_rec);
    pclose(fp_play);
    close(s);
    printf("Server finished.\n");
}

void run_client(const char *ip, uint16_t port) {
    int s = socket(PF_INET, SOCK_STREAM, 0);
    if (s == -1) { perror("socket"); exit(1); }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(ip);
    if (addr.sin_addr.s_addr == INADDR_NONE) {
        fprintf(stderr, "Invalid IP address\n");
        exit(1);
    }
    addr.sin_port = htons(port);

    printf("Connecting to %s:%d...\n", ip, port);
    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("connect"); exit(1);
    }
    printf("Connected. Receiving audio...\n");

    int opt = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    // 録音と再生の両方を起動
    FILE *fp_rec = popen(AUDIO_COMMAND_REC, "r");
    FILE *fp_play = popen(AUDIO_COMMAND_PLAY, "w");
    if (!fp_rec || !fp_play) { perror("popen"); close(s); exit(1); }

    int record_fd = fileno(fp_rec);
    int play_fd = fileno(fp_play);
    unsigned char buffer[BUFFER_SIZE];
    ssize_t n;
    int error_occured = 0;
    int is_muted = 0;

    struct pollfd fds[3];
    // 0: 録音パイプ（読み込み）
    fds[0].fd = record_fd;
    fds[0].events = POLLIN;
    // 1: ソケット（受信）
    fds[1].fd = s;
    fds[1].events = POLLIN;
    // 2: 標準入力（コマンド）
    fds[2].fd = STDIN_FILENO;
    fds[2].events = POLLIN;

    printf("Full-duplex communication started.\n");
    printf("'q'+Enter: quit, 'm'+Enter: toggle microphone mute.\n");

    while (!error_occured) {
        int ret = poll(fds, 3, -1);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }

        // コマンド入力のチェック
        if (fds[2].revents & POLLIN) {
            char cmd[10];
            if (fgets(cmd, sizeof(cmd), stdin)) {
                if (cmd[0] == 'q') {
                    printf("Termination command received. Closing...\n");
                    break;
                } else if (cmd[0] == 'm') {
                    is_muted = !is_muted;
                    printf(">>> Speaker Mute: %s <<<\n", is_muted ? "ON" : "OFF");
                }
            }
        }

        // 1. 自分の声を送る (録音パイプ -> ソケット)
        if (fds[0].revents & (POLLIN | POLLHUP)) {
            n = read(record_fd, buffer, sizeof(buffer));
            if (n <= 0) break;

            if (is_muted) memset(buffer, 0, n);

            ssize_t total_sent = 0;
            while (total_sent < n) {
                ssize_t sent = write(s, buffer + total_sent, n - total_sent);
                if (sent == -1) {
                    if (errno == EINTR) continue;
                    perror("write to socket");
                    error_occured = 1;
                    break;
                }
                total_sent += sent;
            }
        }

        // 2. 相手の声を聞く (ソケット -> 再生パイプ)
        if (fds[1].revents & (POLLIN | POLLHUP)) {
            n = read(s, buffer, sizeof(buffer));
            if (n <= 0) break;

            ssize_t total_played = 0;
            while (total_played < n) {
                ssize_t played = write(play_fd, buffer + total_played, n - total_played);
                if (played == -1) {
                    if (errno == EINTR) continue;
                    perror("write to play process");
                    error_occured = 1;
                    break;
                }
                total_played += played;
            }
        }
    }

    if (n == 0) printf("Connection closed by server.\n");
    else if (n == -1) perror("read from socket");

    pclose(fp_rec);
    pclose(fp_play);
    close(s);
    printf("Client finished.\n");
}