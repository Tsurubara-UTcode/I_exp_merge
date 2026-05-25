/* phone_gui_rtp_opus.c
 *
 * RTP + jitter buffer + Opus + GTK GUI 版 簡易VoIP
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <pthread.h>
#include <math.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <opus.h>
#include <gtk/gtk.h>

#define SAMPLE_RATE 16000
#define CHANNELS 1
#define FRAME_SIZE 320          /* 20 ms at 16 kHz */
#define PCM_BYTES (FRAME_SIZE * 2)
#define MAX_PACKET_SIZE 1500

#define RTP_HEADER_SIZE 12
#define RTP_PAYLOAD_TYPE_OPUS 111

#define JITTER_SIZE 64
#define THRESHOLD 500

typedef struct {
    uint16_t seq;
    uint32_t timestamp;
    uint8_t payload[MAX_PACKET_SIZE];
    int payload_len;
    int used;
} JitterPacket;

typedef struct {
    JitterPacket packets[JITTER_SIZE];
    uint16_t expected_seq;
    int initialized;
    pthread_mutex_t mutex;
} JitterBuffer;

int sock;
struct sockaddr_in peer_addr;

volatile int muted = 0;
volatile int ptt_enabled = 0;
volatile int running = 1;
volatile int last_energy = 0;
volatile int lost_packets = 0;

GtkWidget *label_status;
GtkWidget *label_energy;
GtkWidget *label_loss;

JitterBuffer jitter;

int is_voice(short *samples, int n)
{
    long long energy = 0;

    for (int i = 0; i < n; i++) {
        energy += abs(samples[i]);
    }

    energy /= n;
    last_energy = energy;

    return energy > THRESHOLD;
}

void make_rtp_header(
    uint8_t *packet,
    uint16_t seq,
    uint32_t timestamp,
    uint32_t ssrc
)
{
    packet[0] = 0x80;                  /* RTP version 2 */
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

uint16_t rtp_get_seq(uint8_t *packet)
{
    return ((uint16_t)packet[2] << 8) | packet[3];
}

uint32_t rtp_get_timestamp(uint8_t *packet)
{
    return ((uint32_t)packet[4] << 24) |
           ((uint32_t)packet[5] << 16) |
           ((uint32_t)packet[6] << 8) |
           packet[7];
}

void jitter_init(JitterBuffer *jb)
{
    memset(jb, 0, sizeof(JitterBuffer));
    pthread_mutex_init(&jb->mutex, NULL);
}

void jitter_insert(
    JitterBuffer *jb,
    uint16_t seq,
    uint32_t timestamp,
    uint8_t *payload,
    int payload_len
)
{
    pthread_mutex_lock(&jb->mutex);

    if (!jb->initialized) {
        jb->expected_seq = seq;
        jb->initialized = 1;
    }

    int index = seq % JITTER_SIZE;

    jb->packets[index].seq = seq;
    jb->packets[index].timestamp = timestamp;
    jb->packets[index].payload_len = payload_len;
    jb->packets[index].used = 1;

    memcpy(jb->packets[index].payload, payload, payload_len);

    pthread_mutex_unlock(&jb->mutex);
}

int jitter_pop(
    JitterBuffer *jb,
    uint8_t *payload,
    int *payload_len
)
{
    pthread_mutex_lock(&jb->mutex);

    if (!jb->initialized) {
        pthread_mutex_unlock(&jb->mutex);
        return 0;
    }

    int index = jb->expected_seq % JITTER_SIZE;

    if (jb->packets[index].used &&
        jb->packets[index].seq == jb->expected_seq) {

        *payload_len = jb->packets[index].payload_len;
        memcpy(payload, jb->packets[index].payload, *payload_len);

        jb->packets[index].used = 0;
        jb->expected_seq++;

        pthread_mutex_unlock(&jb->mutex);
        return 1;
    }

    lost_packets++;
    jb->expected_seq++;

    pthread_mutex_unlock(&jb->mutex);
    return 0;
}

void *send_thread(void *arg)
{
    int err;

    OpusEncoder *encoder =
        opus_encoder_create(
            SAMPLE_RATE,
            CHANNELS,
            OPUS_APPLICATION_VOIP,
            &err
        );

    if (err != OPUS_OK) {
        fprintf(stderr, "Opus encoder error\n");
        return NULL;
    }

    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(24000));
    opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

    FILE *rec =
        popen(
            "rec -q "
            "-t raw "
            "-b 16 "
            "-c 1 "
            "-e s "
            "-r 16000 - "
            "highpass 100 "
            "lowpass 3400",
            "r"
        );

    if (!rec) {
        perror("rec");
        return NULL;
    }

    uint16_t seq = 0;
    uint32_t timestamp = 0;
    uint32_t ssrc = 0x12345678;

    short pcm[FRAME_SIZE];
    uint8_t opus_data[MAX_PACKET_SIZE];
    uint8_t packet[MAX_PACKET_SIZE];

    while (running) {
        size_t n =
            fread(pcm, sizeof(short), FRAME_SIZE, rec);

        if (n != FRAME_SIZE) {
            continue;
        }

        if (muted) {
            continue;
        }

        if (!ptt_enabled) {
            continue;
        }

        if (!is_voice(pcm, FRAME_SIZE)) {
            continue;
        }

        int opus_len =
            opus_encode(
                encoder,
                pcm,
                FRAME_SIZE,
                opus_data,
                sizeof(opus_data)
            );

        if (opus_len < 0) {
            continue;
        }

        make_rtp_header(packet, seq, timestamp, ssrc);

        memcpy(packet + RTP_HEADER_SIZE,
               opus_data,
               opus_len);

        sendto(
            sock,
            packet,
            RTP_HEADER_SIZE + opus_len,
            0,
            (struct sockaddr *)&peer_addr,
            sizeof(peer_addr)
        );

        seq++;
        timestamp += FRAME_SIZE;
    }

    opus_encoder_destroy(encoder);
    pclose(rec);

    return NULL;
}

void *recv_thread(void *arg)
{
    uint8_t packet[MAX_PACKET_SIZE];

    while (running) {
        ssize_t n =
            recvfrom(
                sock,
                packet,
                sizeof(packet),
                0,
                NULL,
                NULL
            );

        if (n <= RTP_HEADER_SIZE) {
            continue;
        }

        uint16_t seq = rtp_get_seq(packet);
        uint32_t timestamp = rtp_get_timestamp(packet);

        uint8_t *payload = packet + RTP_HEADER_SIZE;
        int payload_len = n - RTP_HEADER_SIZE;

        jitter_insert(
            &jitter,
            seq,
            timestamp,
            payload,
            payload_len
        );
    }

    return NULL;
}

void *play_thread(void *arg)
{
    int err;

    OpusDecoder *decoder =
        opus_decoder_create(
            SAMPLE_RATE,
            CHANNELS,
            &err
        );

    if (err != OPUS_OK) {
        fprintf(stderr, "Opus decoder error\n");
        return NULL;
    }

    FILE *play =
        popen(
            "play -q "
            "-t raw "
            "-b 16 "
            "-c 1 "
            "-e s "
            "-r 16000 -",
            "w"
        );

    FILE *record =
        fopen("call_decoded.raw", "wb");

    uint8_t opus_payload[MAX_PACKET_SIZE];
    int opus_len;
    short pcm[FRAME_SIZE];

    usleep(80000);  /* jitter buffer warmup */

    while (running) {
        int has_packet =
            jitter_pop(
                &jitter,
                opus_payload,
                &opus_len
            );

        int decoded;

        if (has_packet) {
            decoded =
                opus_decode(
                    decoder,
                    opus_payload,
                    opus_len,
                    pcm,
                    FRAME_SIZE,
                    0
                );
        } else {
            decoded =
                opus_decode(
                    decoder,
                    NULL,
                    0,
                    pcm,
                    FRAME_SIZE,
                    0
                );
        }

        if (decoded > 0) {
            fwrite(pcm, sizeof(short), decoded, play);
            fflush(play);

            if (record) {
                fwrite(pcm, sizeof(short), decoded, record);
            }
        }

        usleep(20000);
    }

    opus_decoder_destroy(decoder);

    if (record) {
        fclose(record);
    }

    pclose(play);

    return NULL;
}

gboolean update_gui(gpointer data)
{
    char buf[128];

    snprintf(buf, sizeof(buf),
             "Status: %s / %s",
             muted ? "Muted" : "Mic ON",
             ptt_enabled ? "PTT ON" : "PTT OFF");

    gtk_label_set_text(GTK_LABEL(label_status), buf);

    snprintf(buf, sizeof(buf),
             "Input Energy: %d",
             last_energy);

    gtk_label_set_text(GTK_LABEL(label_energy), buf);

    snprintf(buf, sizeof(buf),
             "Estimated Lost Packets: %d",
             lost_packets);

    gtk_label_set_text(GTK_LABEL(label_loss), buf);

    return TRUE;
}

void on_mute_clicked(GtkWidget *widget, gpointer data)
{
    muted = !muted;
}

void on_ptt_pressed(GtkWidget *widget, gpointer data)
{
    ptt_enabled = 1;
}

void on_ptt_released(GtkWidget *widget, gpointer data)
{
    ptt_enabled = 0;
}

void on_window_destroy(GtkWidget *widget, gpointer data)
{
    running = 0;
    gtk_main_quit();
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr,
                "Usage: %s my_port peer_ip peer_port\n",
                argv[0]);
        exit(1);
    }

    int my_port = atoi(argv[1]);
    char *peer_ip = argv[2];
    int peer_port = atoi(argv[3]);

    sock = socket(AF_INET, SOCK_DGRAM, 0);

    if (sock < 0) {
        perror("socket");
        exit(1);
    }

    struct sockaddr_in my_addr;

    memset(&my_addr, 0, sizeof(my_addr));
    memset(&peer_addr, 0, sizeof(peer_addr));

    my_addr.sin_family = AF_INET;
    my_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    my_addr.sin_port = htons(my_port);

    if (bind(sock,
             (struct sockaddr *)&my_addr,
             sizeof(my_addr)) < 0) {
        perror("bind");
        exit(1);
    }

    peer_addr.sin_family = AF_INET;
    peer_addr.sin_addr.s_addr = inet_addr(peer_ip);
    peer_addr.sin_port = htons(peer_port);

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
                         "Simple RTP Opus Phone");

    gtk_window_set_default_size(GTK_WINDOW(window),
                                360,
                                220);

    GtkWidget *box =
        gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);

    gtk_container_set_border_width(GTK_CONTAINER(box), 20);
    gtk_container_add(GTK_CONTAINER(window), box);

    label_status = gtk_label_new("Status");
    label_energy = gtk_label_new("Input Energy");
    label_loss = gtk_label_new("Packet Loss");

    GtkWidget *button_mute =
        gtk_button_new_with_label("Mute ON/OFF");

    GtkWidget *button_ptt =
        gtk_button_new_with_label("Push To Talk");

    gtk_box_pack_start(GTK_BOX(box), label_status, FALSE, FALSE, 5);
    gtk_box_pack_start(GTK_BOX(box), label_energy, FALSE, FALSE, 5);
    gtk_box_pack_start(GTK_BOX(box), label_loss, FALSE, FALSE, 5);
    gtk_box_pack_start(GTK_BOX(box), button_mute, FALSE, FALSE, 5);
    gtk_box_pack_start(GTK_BOX(box), button_ptt, FALSE, FALSE, 5);

    g_signal_connect(window,
                     "destroy",
                     G_CALLBACK(on_window_destroy),
                     NULL);

    g_signal_connect(button_mute,
                     "clicked",
                     G_CALLBACK(on_mute_clicked),
                     NULL);

    g_signal_connect(button_ptt,
                     "pressed",
                     G_CALLBACK(on_ptt_pressed),
                     NULL);

    g_signal_connect(button_ptt,
                     "released",
                     G_CALLBACK(on_ptt_released),
                     NULL);

    g_timeout_add(200, update_gui, NULL);

    gtk_widget_show_all(window);
    gtk_main();

    close(sock);

    return 0;
}