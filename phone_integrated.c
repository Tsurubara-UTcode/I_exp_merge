/* phone_integrated.c
 * UDP + RTP + Opus + jitter buffer + GTK GUI VoIP
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <math.h>
#include <time.h>
#include <errno.h>

#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <gtk/gtk.h>
#include <opus/opus.h>

#define SAMPLE_RATE 48000
#define CHANNELS 1
#define FRAME_SIZE 960
#define RTP_HEADER_SIZE 12
#define MAX_PACKET_SIZE 1500
#define OPUS_MAX_BYTES 4000
#define JITTER_SIZE 64
#define THRESHOLD 500

#define AUDIO_COMMAND_REC  "rec -t pulse -q -t raw -b 16 -e signed-integer -c 1 -r 48000 - highpass 100 lowpass 3400"
#define AUDIO_COMMAND_PLAY "play -t pulse -q -t raw -b 16 -e signed-integer -c 1 -r 48000 -"

#define RTP_PAYLOAD_TYPE_OPUS 111

typedef struct {
    uint16_t seq;
    int used;
    int len;
    unsigned char payload[OPUS_MAX_BYTES];
} JitterPacket;

typedef struct {
    JitterPacket packets[JITTER_SIZE];
    uint16_t expected_seq;
    int initialized;
    pthread_mutex_t mutex;
} JitterBuffer;

static int sockfd;
static struct sockaddr_in peer_addr;

static volatile int running = 1;
static volatile int muted = 0;
static volatile int ptt = 0;
static volatile int last_energy = 0;
static volatile int lost_packets = 0;
static volatile int sent_packets = 0;
static volatile int received_packets = 0;

static JitterBuffer jitter;

static GtkWidget *label_status;
static GtkWidget *label_energy;
static GtkWidget *label_packets;
static GtkWidget *label_loss;

int is_voice(short *pcm, int n)
{
    long long energy = 0;

    for (int i = 0; i < n; i++) {
        energy += abs(pcm[i]);
    }

    energy /= n;
    last_energy = (int)energy;

    return energy > THRESHOLD;
}

void make_rtp_header(unsigned char *packet,
                     uint16_t seq,
                     uint32_t timestamp,
                     uint32_t ssrc)
{
    packet[0] = 0x80;
    packet[1] = RTP_PAYLOAD_TYPE_OPUS;

    packet[2] = seq >> 8;
    packet[3] = seq & 0xff;

    packet[4] = timestamp >> 24;
    packet[5] = timestamp >> 16;
    packet[6] = timestamp >> 8;
    packet[7] = timestamp & 0xff;

    packet[8] = ssrc >> 24;
    packet[9] = ssrc >> 16;
    packet[10] = ssrc >> 8;
    packet[11] = ssrc & 0xff;
}

uint16_t rtp_get_seq(unsigned char *packet)
{
    return ((uint16_t)packet[2] << 8) | packet[3];
}

void jitter_init(JitterBuffer *jb)
{
    memset(jb, 0, sizeof(JitterBuffer));
    pthread_mutex_init(&jb->mutex, NULL);
}

void jitter_insert(JitterBuffer *jb,
                   uint16_t seq,
                   unsigned char *payload,
                   int len)
{
    pthread_mutex_lock(&jb->mutex);

    if (!jb->initialized) {
        jb->expected_seq = seq;
        jb->initialized = 1;
    }

    int index = seq % JITTER_SIZE;

    jb->packets[index].seq = seq;
    jb->packets[index].used = 1;
    jb->packets[index].len = len;

    if (len > OPUS_MAX_BYTES) {
        len = OPUS_MAX_BYTES;
    }

    memcpy(jb->packets[index].payload, payload, len);

    pthread_mutex_unlock(&jb->mutex);
}

int jitter_pop(JitterBuffer *jb,
               unsigned char *payload,
               int *len)
{
    pthread_mutex_lock(&jb->mutex);

    if (!jb->initialized) {
        pthread_mutex_unlock(&jb->mutex);
        return 0;
    }

    int index = jb->expected_seq % JITTER_SIZE;

    if (jb->packets[index].used &&
        jb->packets[index].seq == jb->expected_seq) {

        *len = jb->packets[index].len;
        memcpy(payload,
               jb->packets[index].payload,
               *len);

        jb->packets[index].used = 0;
        jb->expected_seq++;

        pthread_mutex_unlock(&jb->mutex);
        return 1;
    }

    /*
     * バッファ内に他のパケットが存在するか確認する。
     * 存在する → アクティブ受信中の真のロスト → カウントして seq を進める
     * 存在しない → PTT無音期間など → カウントも seq も進めない
     *   (seq を進めないことで、次の送信セッションと再同期できる)
     */
    int has_any = 0;
    for (int i = 0; i < JITTER_SIZE; i++) {
        if (jb->packets[i].used) {
            has_any = 1;
            break;
        }
    }

    if (has_any) {
        lost_packets++;
        jb->expected_seq++;
    }

    pthread_mutex_unlock(&jb->mutex);
    return 0;
}

/* 指定シーケンス番号のパケットをバッファから取り出さずに読む（FEC用） */
int jitter_peek(JitterBuffer *jb,
                uint16_t seq,
                unsigned char *payload,
                int *len)
{
    pthread_mutex_lock(&jb->mutex);

    int index = seq % JITTER_SIZE;

    if (jb->packets[index].used &&
        jb->packets[index].seq == seq) {

        *len = jb->packets[index].len;
        memcpy(payload, jb->packets[index].payload, *len);

        pthread_mutex_unlock(&jb->mutex);
        return 1;
    }

    pthread_mutex_unlock(&jb->mutex);
    return 0;
}

void *send_thread(void *arg)
{
    int err;

    OpusEncoder *encoder =
        opus_encoder_create(SAMPLE_RATE,
                            CHANNELS,
                            OPUS_APPLICATION_VOIP,
                            &err);

    if (err != OPUS_OK) {
        fprintf(stderr, "opus_encoder_create failed: %s\n",
                opus_strerror(err));
        return NULL;
    }

    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(24000));
    opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(encoder, OPUS_SET_DTX(1));
    opus_encoder_ctl(encoder, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(encoder, OPUS_SET_PACKET_LOSS_PERC(10));

    FILE *rec = popen(AUDIO_COMMAND_REC, "r");

    if (!rec) {
        perror("popen rec");
        opus_encoder_destroy(encoder);
        return NULL;
    }

    short pcm[FRAME_SIZE];
    unsigned char opus_data[OPUS_MAX_BYTES];
    unsigned char packet[MAX_PACKET_SIZE];

    uint16_t seq = 0;
    uint32_t timestamp = 0;
    uint32_t ssrc = 0x12345678;

    while (running) {
        /* 100ms ごとに running を確認し、終了時に fread の永久ブロックを防ぐ */
        fd_set rfds;
        struct timeval tv = {0, 100000};
        int fd = fileno(rec);
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        if (select(fd + 1, &rfds, NULL, NULL, &tv) <= 0) {
            continue; /* タイムアウトまたはエラー → ループ先頭で running を再確認 */
        }

        size_t n = fread(pcm,
                         sizeof(short),
                         FRAME_SIZE,
                         rec);

        if (n != FRAME_SIZE) {
            if (feof(rec)) break;
            continue;
        }

        if (muted) {
            continue;
        }

        if (!ptt) {
            continue;
        }

        /* GUIのエネルギー表示だけ更新し、送信判定はOpus DTXに任せる */
        is_voice(pcm, FRAME_SIZE);

        int opus_len =
            opus_encode(encoder,
                        pcm,
                        FRAME_SIZE,
                        opus_data,
                        sizeof(opus_data));

        if (opus_len < 0) {
            fprintf(stderr, "opus_encode failed: %s\n",
                    opus_strerror(opus_len));
            continue;
        }

        make_rtp_header(packet,
                        seq,
                        timestamp,
                        ssrc);

        memcpy(packet + RTP_HEADER_SIZE,
               opus_data,
               opus_len);

        ssize_t sent =
            sendto(sockfd,
                   packet,
                   RTP_HEADER_SIZE + opus_len,
                   0,
                   (struct sockaddr *)&peer_addr,
                   sizeof(peer_addr));

        if (sent < 0) {
            perror("sendto");
        } else {
            sent_packets++;
        }

        seq++;
        timestamp += FRAME_SIZE;
    }

    pclose(rec);
    opus_encoder_destroy(encoder);

    return NULL;
}

void *recv_thread(void *arg)
{
    unsigned char packet[MAX_PACKET_SIZE];

    while (running) {
        ssize_t n =
            recvfrom(sockfd,
                     packet,
                     sizeof(packet),
                     0,
                     NULL,
                     NULL);

        if (n <= RTP_HEADER_SIZE) {
            continue;
        }

        uint16_t seq = rtp_get_seq(packet);
        int payload_len = (int)n - RTP_HEADER_SIZE;

        jitter_insert(&jitter,
                      seq,
                      packet + RTP_HEADER_SIZE,
                      payload_len);

        received_packets++;
    }

    return NULL;
}

void *play_thread(void *arg)
{
    int err;

    OpusDecoder *decoder =
        opus_decoder_create(SAMPLE_RATE,
                            CHANNELS,
                            &err);

    if (err != OPUS_OK) {
        fprintf(stderr, "opus_decoder_create failed: %s\n",
                opus_strerror(err));
        return NULL;
    }

    FILE *play = popen(AUDIO_COMMAND_PLAY, "w");
    FILE *record = fopen("call_decoded.raw", "wb");

    if (!play) {
        perror("popen play");
        opus_decoder_destroy(decoder);
        return NULL;
    }

    unsigned char opus_payload[OPUS_MAX_BYTES];
    int opus_len;
    short pcm[FRAME_SIZE];

    usleep(80000);

    /* 絶対時刻ベースのタイマー初期化 */
    struct timespec next_time;
    clock_gettime(CLOCK_MONOTONIC, &next_time);

    while (running) {
        int has_packet =
            jitter_pop(&jitter,
                       opus_payload,
                       &opus_len);

        int decoded;

        if (has_packet) {
            /* 正常デコード */
            decoded =
                opus_decode(decoder,
                            opus_payload,
                            opus_len,
                            pcm,
                            FRAME_SIZE,
                            0);
        } else {
            /* ロスト: 次パケットのFECで前フレームを復元を試みる */
            unsigned char next_payload[OPUS_MAX_BYTES];
            int next_len;

            if (jitter_peek(&jitter,
                            jitter.expected_seq,
                            next_payload,
                            &next_len)) {
                /* 次パケットのFECデータで補完（バッファは消費しない） */
                decoded =
                    opus_decode(decoder,
                                next_payload,
                                next_len,
                                pcm,
                                FRAME_SIZE,
                                1);
            } else {
                /* 次パケットも未着: PLCにフォールバック */
                decoded =
                    opus_decode(decoder,
                                NULL,
                                0,
                                pcm,
                                FRAME_SIZE,
                                0);
            }
        }

        if (decoded > 0) {
            fwrite(pcm,
                   sizeof(short),
                   decoded,
                   play);

            fflush(play);

            if (record) {
                fwrite(pcm,
                       sizeof(short),
                       decoded,
                       record);
            }
        }

        /* 絶対時刻ベースで20ms間隔を維持（処理時間のドリフトを吸収） */
        next_time.tv_nsec += 20000000L;
        if (next_time.tv_nsec >= 1000000000L) {
            next_time.tv_sec  += 1;
            next_time.tv_nsec -= 1000000000L;
        }
        /* macOSはclock_nanosleepが非対応のため、残り時間を計算してnanosleep */
        {
            struct timespec now, remaining;
            clock_gettime(CLOCK_MONOTONIC, &now);
            remaining.tv_sec  = next_time.tv_sec  - now.tv_sec;
            remaining.tv_nsec = next_time.tv_nsec - now.tv_nsec;
            if (remaining.tv_nsec < 0) {
                remaining.tv_sec  -= 1;
                remaining.tv_nsec += 1000000000L;
            }
            if (remaining.tv_sec >= 0 && remaining.tv_nsec >= 0) {
                nanosleep(&remaining, NULL);
            }
        }
    }

    if (record) fclose(record);
    pclose(play);
    opus_decoder_destroy(decoder);

    return NULL;
}

gboolean update_gui(gpointer data)
{
    char buf[128];

    snprintf(buf, sizeof(buf),
             "Mic: %s / Push-To-Talk: %s",
             muted ? "Muted" : "ON",
             ptt ? "ON" : "OFF");

    gtk_label_set_text(GTK_LABEL(label_status), buf);

    snprintf(buf, sizeof(buf),
             "Input Energy: %d",
             last_energy);

    gtk_label_set_text(GTK_LABEL(label_energy), buf);

    snprintf(buf, sizeof(buf),
             "Sent: %d / Received: %d",
             sent_packets,
             received_packets);

    gtk_label_set_text(GTK_LABEL(label_packets), buf);

    snprintf(buf, sizeof(buf),
             "Lost or PLC packets: %d",
             lost_packets);

    gtk_label_set_text(GTK_LABEL(label_loss), buf);

    return TRUE;
}

void on_mute_clicked(GtkWidget *widget, gpointer data)
{
    muted = !muted;
}

void on_ptt_toggled(GtkWidget *widget, gpointer data)
{
    ptt = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
}

void on_destroy(GtkWidget *widget, gpointer data)
{
    running = 0;
    close(sockfd);
    gtk_main_quit();
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);

    if (argc != 4) {
        fprintf(stderr,
                "Usage: %s my_port peer_ip peer_port\n",
                argv[0]);
        exit(1);
    }

    int my_port = atoi(argv[1]);
    const char *peer_ip = argv[2];
    int peer_port = atoi(argv[3]);

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);

    if (sockfd < 0) {
        perror("socket");
        exit(1);
    }

    struct sockaddr_in my_addr;

    memset(&my_addr, 0, sizeof(my_addr));
    memset(&peer_addr, 0, sizeof(peer_addr));

    my_addr.sin_family = AF_INET;
    my_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    my_addr.sin_port = htons(my_port);

    if (bind(sockfd,
             (struct sockaddr *)&my_addr,
             sizeof(my_addr)) < 0) {
        perror("bind");
        close(sockfd);
        exit(1);
    }

    peer_addr.sin_family = AF_INET;
    peer_addr.sin_port = htons(peer_port);

    if (inet_aton(peer_ip, &peer_addr.sin_addr) == 0) {
        fprintf(stderr, "Invalid peer IP address\n");
        close(sockfd);
        exit(1);
    }

    jitter_init(&jitter);

    pthread_t th_send;
    pthread_t th_recv;
    pthread_t th_play;

    pthread_create(&th_send, NULL, send_thread, NULL);
    pthread_create(&th_recv, NULL, recv_thread, NULL);
    pthread_create(&th_play, NULL, play_thread, NULL);

    gtk_init(&argc, &argv);

    GtkWidget *window =
        gtk_window_new(GTK_WINDOW_TOPLEVEL);

    gtk_window_set_title(GTK_WINDOW(window),
                         "Integrated RTP Opus VoIP");

    gtk_window_set_default_size(GTK_WINDOW(window),
                                420,
                                260);

    GtkWidget *box =
        gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);

    gtk_container_set_border_width(GTK_CONTAINER(box), 20);
    gtk_container_add(GTK_CONTAINER(window), box);

    label_status = gtk_label_new("Mic: ON / Push-To-Talk: OFF");
    label_energy = gtk_label_new("Input Energy: 0");
    label_packets = gtk_label_new("Sent: 0 / Received: 0");
    label_loss = gtk_label_new("Lost or PLC packets: 0");

    GtkWidget *button_mute =
        gtk_button_new_with_label("Mute ON/OFF");

    GtkWidget *button_ptt =
        gtk_toggle_button_new_with_label("Push To Talk");

    gtk_box_pack_start(GTK_BOX(box), label_status, FALSE, FALSE, 5);
    gtk_box_pack_start(GTK_BOX(box), label_energy, FALSE, FALSE, 5);
    gtk_box_pack_start(GTK_BOX(box), label_packets, FALSE, FALSE, 5);
    gtk_box_pack_start(GTK_BOX(box), label_loss, FALSE, FALSE, 5);
    gtk_box_pack_start(GTK_BOX(box), button_mute, FALSE, FALSE, 5);
    gtk_box_pack_start(GTK_BOX(box), button_ptt, FALSE, FALSE, 5);

    g_signal_connect(window,
                     "destroy",
                     G_CALLBACK(on_destroy),
                     NULL);

    g_signal_connect(button_mute,
                     "clicked",
                     G_CALLBACK(on_mute_clicked),
                     NULL);

    g_signal_connect(button_ptt,
                     "toggled",
                     G_CALLBACK(on_ptt_toggled),
                     NULL);

    g_timeout_add(200, update_gui, NULL);

    gtk_widget_show_all(window);
    gtk_main();

    running = 0;

    /* スレッドが pclose() で rec/play を正常終了させてから exit する */
    pthread_join(th_send, NULL);
    pthread_join(th_recv, NULL);
    pthread_join(th_play, NULL);

    return 0;
}
