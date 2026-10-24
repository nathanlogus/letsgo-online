// Native client for the small social co-op relay (protocol in docs/COOP-ONLINE.md).
// The worker never touches game memory. Game hooks publish a copied pose; rendering reads a copied peer pose.
#include "coop_net.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "rt.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET CoopSocket;
#define COOP_INVALID_SOCKET INVALID_SOCKET
#define coop_close closesocket
#define coop_shutdown(s) shutdown((s), SD_BOTH)
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int CoopSocket;
#define COOP_INVALID_SOCKET (-1)
#define coop_close close
#define coop_shutdown(s) shutdown((s), SHUT_RDWR)
#endif

#define COOP_MAX_FRAME 256
#define COOP_SNAPSHOT_MS 66
#define COOP_PING_MS 10000

typedef struct {
    bool valid;
    float x, y, z, yaw;
    uint32_t area;
    uint8_t motion;
} LocalPose;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_thread;
static bool g_thread_started;
static bool g_configured;
static _Atomic bool g_stop;
static CoopSocket g_socket = COOP_INVALID_SOCKET;
static LocalPose g_local;
static CoopRemote g_remote;
static uint8_t g_pending_emote;
static bool g_has_pending_emote;
static char g_room_code[17];
static uint32_t g_sequence;

static uint16_t get_u16(const uint8_t *p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
static uint32_t get_u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t get_u64(const uint8_t *p) { return (uint64_t)get_u32(p) | (uint64_t)get_u32(p + 4) << 32; }
static void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_u32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (i * 8)); }
static void put_u64(uint8_t *p, uint64_t v) { put_u32(p, (uint32_t)v); put_u32(p + 4, (uint32_t)(v >> 32)); }

static bool send_all(CoopSocket s, const uint8_t *p, size_t n) {
    while (n) {
#ifdef _WIN32
        int sent = send(s, (const char *)p, (int)n, 0);
#else
        ssize_t sent = send(s, p, n, 0);
#endif
        if (sent <= 0) return false;
        p += sent;
        n -= (size_t)sent;
    }
    return true;
}

static bool recv_all(CoopSocket s, uint8_t *p, size_t n) {
    while (n) {
#ifdef _WIN32
        int got = recv(s, (char *)p, (int)n, 0);
#else
        ssize_t got = recv(s, p, n, 0);
#endif
        if (got <= 0) return false;
        p += got;
        n -= (size_t)got;
    }
    return true;
}

static bool send_frame(CoopSocket s, uint8_t op, const uint8_t *body, size_t n) {
    if (n + 1 > COOP_MAX_FRAME) return false;
    uint8_t packet[COOP_MAX_FRAME + 2];
    put_u16(packet, (uint16_t)(n + 1));
    packet[2] = op;
    if (n) memcpy(packet + 3, body, n);
    return send_all(s, packet, n + 3);
}

static bool recv_frame(CoopSocket s, uint8_t *packet, size_t *n) {
    uint8_t hdr[2];
    if (!recv_all(s, hdr, sizeof hdr)) return false;
    *n = get_u16(hdr);
    if (!*n || *n > COOP_MAX_FRAME) return false;
    return recv_all(s, packet, *n);
}

static bool parse_server(const char *src, char *host, size_t host_size, char *port, size_t port_size) {
    if (!src || !*src) return false;
    const char *colon = strrchr(src, ':');
    size_t n = colon ? (size_t)(colon - src) : strlen(src);
    if (!n || n >= host_size) return false;
    memcpy(host, src, n);
    host[n] = 0;
    snprintf(port, port_size, "%s", colon && colon[1] ? colon + 1 : "7381");
    return true;
}

static bool main_build_id(uint64_t *id) {
    FILE *f = fopen("extracted/exefs/main", "rb");
    if (!f) return false;
    uint8_t b[8];
    bool ok = !fseek(f, 0x40, SEEK_SET) && fread(b, 1, sizeof b, f) == sizeof b;
    fclose(f);
    if (ok) *id = get_u64(b);
    return ok;
}

static CoopSocket connect_to(const char *host, const char *port) {
    struct addrinfo hints = {0}, *list = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &list) != 0) return COOP_INVALID_SOCKET;
    CoopSocket result = COOP_INVALID_SOCKET;
    for (struct addrinfo *a = list; a; a = a->ai_next) {
        CoopSocket s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == COOP_INVALID_SOCKET) continue;
        if (connect(s, a->ai_addr, (int)a->ai_addrlen) == 0) { result = s; break; }
        coop_close(s);
    }
    freeaddrinfo(list);
    return result;
}

static int32_t quantize_cm(float v) {
    if (!isfinite(v)) return 0;
    double d = v;
    if (d > 20000000.0) d = 20000000.0;
    if (d < -20000000.0) d = -20000000.0;
    return (int32_t)lrint(d);
}

static uint16_t quantize_yaw(float v) {
    if (!isfinite(v)) return 0;
    double turns = (double)v / (2.0 * 3.14159265358979323846);
    turns -= floor(turns);
    return (uint16_t)lrint(turns * 65535.0);
}

static void set_socket(CoopSocket s) {
    pthread_mutex_lock(&g_lock);
    g_socket = s;
    pthread_mutex_unlock(&g_lock);
}

static void clear_remote(void) {
    pthread_mutex_lock(&g_lock);
    memset(&g_remote, 0, sizeof g_remote);
    pthread_mutex_unlock(&g_lock);
}

static void process_message(const uint8_t *p, size_t n) {
    if (!n) return;
    if (p[0] == 'A' && n >= 3) {
        uint8_t slot = p[1], nc = p[2];
        if (slot > 1 || nc > 16 || (size_t)nc + 3 != n) return;
        pthread_mutex_lock(&g_lock);
        memcpy(g_room_code, p + 3, nc);
        g_room_code[nc] = 0;
        pthread_mutex_unlock(&g_lock);
        LOG("[coop] joined room %s as player %u\n", g_room_code, slot + 1);
        return;
    }
    if (p[0] == 'P' && n >= 3) {
        uint8_t slot = p[1];
        if (slot > 1 || (size_t)p[2] + 3 != n || p[2] > 48) return;
        pthread_mutex_lock(&g_lock);
        g_remote.present = true;
        g_remote.slot = slot;
        size_t len = p[2] < sizeof g_remote.name - 1 ? p[2] : sizeof g_remote.name - 1;
        memcpy(g_remote.name, p + 3, len);
        g_remote.name[len] = 0;
        pthread_mutex_unlock(&g_lock);
        LOG("[coop] %s joined the room\n", g_remote.name);
        return;
    }
    if (p[0] == 'L' && n == 2) {
        clear_remote();
        LOG("[coop] the other player left\n");
        return;
    }
    if (p[0] == 'S' && n == 25) {  // slot + 23-byte snapshot
        pthread_mutex_lock(&g_lock);
        if (g_remote.present && p[1] == g_remote.slot) {
            const uint8_t *b = p + 2;
            g_remote.area = get_u32(b);
            g_remote.sequence = get_u32(b + 4);
            g_remote.x = (float)(int32_t)get_u32(b + 8);
            g_remote.y = (float)(int32_t)get_u32(b + 12);
            g_remote.z = (float)(int32_t)get_u32(b + 16);
            g_remote.yaw = (float)get_u16(b + 20) * (2.0f * 3.14159265358979323846f / 65535.0f);
            g_remote.motion = b[22];
            g_remote.received_ns = plat_now_ns();
        }
        pthread_mutex_unlock(&g_lock);
        return;
    }
    if (p[0] == 'E' && n == 3) {
        LOG("[coop] player %u sent emote %u\n", p[1] + 1, p[2]);
        return;
    }
    if (p[0] == 'X') {
        size_t len = n >= 2 && p[1] <= n - 2 ? p[1] : n - 1;
        char why[100];
        if (len >= sizeof why) len = sizeof why - 1;
        memcpy(why, p + (n >= 2 ? 2 : 1), len);
        why[len] = 0;
        LOG("[coop] relay refused connection: %s\n", why);
    }
}

static void *coop_worker(void *unused) {
    (void)unused;
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { LOG("[coop] Winsock initialization failed\n"); return NULL; }
#endif
    char host[256], port[16];
    const char *server = getenv("LARECOMP_COOP_SERVER");
    if (!parse_server(server, host, sizeof host, port, sizeof port)) {
        LOG("[coop] set LARECOMP_COOP_SERVER=host:port to connect\n");
        goto done;
    }
    uint64_t build_id = 0;
    if (!main_build_id(&build_id)) {
        LOG("[coop] couldn't read extracted/exefs/main for compatibility check\n");
        goto done;
    }
    const char *name = getenv("LARECOMP_COOP_NAME");
    if (!name || !*name) name = "Trainer";
    size_t name_len = strlen(name);
    if (name_len > 48) name_len = 48;
    bool create = getenv("LARECOMP_COOP_HOST") && !strcmp(getenv("LARECOMP_COOP_HOST"), "1");
    const char *room = getenv("LARECOMP_COOP_ROOM");
    size_t room_len = room ? strlen(room) : 0;
    if (!create && (!room_len || room_len > 16)) {
        LOG("[coop] join mode needs LARECOMP_COOP_ROOM\n");
        goto done;
    }

    LOG("[coop] connecting to %s:%s\n", host, port);
    CoopSocket s = connect_to(host, port);
    if (s == COOP_INVALID_SOCKET) { LOG("[coop] couldn't connect to relay\n"); goto done; }
    set_socket(s);
    uint8_t hello[128];
    size_t n = 0;
    hello[n++] = create ? 'C' : 'J';
    hello[n++] = 1;  // wire protocol version
    put_u64(hello + n, g_program_id); n += 8;
    put_u64(hello + n, build_id); n += 8;
    if (!create) {
        hello[n++] = (uint8_t)room_len;
        memcpy(hello + n, room, room_len); n += room_len;
    }
    hello[n++] = (uint8_t)name_len;
    memcpy(hello + n, name, name_len); n += name_len;
    if (!send_frame(s, hello[0], hello + 1, n - 1)) { LOG("[coop] couldn't send room request\n"); goto close_socket; }

    uint64_t last_ping = plat_now_ns(), last_snapshot = 0;
    while (!atomic_load(&g_stop)) {
        uint64_t now = plat_now_ns();
        if (now - last_ping >= COOP_PING_MS * 1000000ull) {
            uint8_t ping[4]; put_u32(ping, (uint32_t)(now / 1000000ull));
            if (!send_frame(s, 'Q', ping, sizeof ping)) break;
            last_ping = now;
        }

        LocalPose pose;
        uint8_t emote = 0;
        bool send_emote = false;
        pthread_mutex_lock(&g_lock);
        pose = g_local;
        if (g_has_pending_emote) { emote = g_pending_emote; g_has_pending_emote = false; send_emote = true; }
        pthread_mutex_unlock(&g_lock);
        if (pose.valid && now - last_snapshot >= COOP_SNAPSHOT_MS * 1000000ull) {
            uint8_t body[23];
            put_u32(body, pose.area);
            put_u32(body + 4, ++g_sequence);
            put_u32(body + 8, (uint32_t)quantize_cm(pose.x));
            put_u32(body + 12, (uint32_t)quantize_cm(pose.y));
            put_u32(body + 16, (uint32_t)quantize_cm(pose.z));
            put_u16(body + 20, quantize_yaw(pose.yaw));
            body[22] = pose.motion;
            if (!send_frame(s, 'S', body, sizeof body)) break;
            last_snapshot = now;
        }
        if (send_emote && !send_frame(s, 'E', &emote, 1)) break;

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(s, &readfds);
        struct timeval timeout = {0, 50000};
#ifdef _WIN32
        int ready = select(0, &readfds, NULL, NULL, &timeout);
#else
        int ready = select(s + 1, &readfds, NULL, NULL, &timeout);
#endif
        if (ready < 0) break;
        if (!ready) continue;
        uint8_t packet[COOP_MAX_FRAME];
        size_t got;
        if (!recv_frame(s, packet, &got)) break;
        process_message(packet, got);
    }
close_socket:
    set_socket(COOP_INVALID_SOCKET);
    coop_shutdown(s);
    coop_close(s);
    clear_remote();
done:
#ifdef _WIN32
    WSACleanup();
#endif
    LOG("[coop] network worker stopped\n");
    return NULL;
}

void coop_net_start(void) {
    if (g_thread_started) return;
    const char *server = getenv("LARECOMP_COOP_SERVER");
    if (!server || !*server) return;
    g_configured = true;
    atomic_store(&g_stop, false);
    pthread_mutex_lock(&g_lock);
    memset(&g_local, 0, sizeof g_local);
    memset(&g_remote, 0, sizeof g_remote);
    g_has_pending_emote = false;
    pthread_mutex_unlock(&g_lock);
    if (pthread_create(&g_thread, NULL, coop_worker, NULL)) {
        g_configured = false;
        LOG("[coop] couldn't start the network worker\n");
        return;
    }
    g_thread_started = true;
}

bool coop_net_enabled(void) { return g_configured; }

void coop_net_reset(void) {
    if (!g_thread_started) { g_configured = false; return; }
    atomic_store(&g_stop, true);
    pthread_mutex_lock(&g_lock);
    CoopSocket s = g_socket;
    pthread_mutex_unlock(&g_lock);
    if (s != COOP_INVALID_SOCKET) coop_shutdown(s);
    pthread_join(g_thread, NULL);
    g_thread_started = false;
    g_configured = false;
    pthread_mutex_lock(&g_lock);
    g_socket = COOP_INVALID_SOCKET;
    memset(&g_local, 0, sizeof g_local);
    memset(&g_remote, 0, sizeof g_remote);
    g_room_code[0] = 0;
    pthread_mutex_unlock(&g_lock);
}

void coop_net_publish_position(float x, float y, float z, float yaw, uint32_t area, uint8_t motion) {
    if (!g_configured) return;
    pthread_mutex_lock(&g_lock);
    g_local = (LocalPose){.valid = true, .x = x, .y = y, .z = z, .yaw = yaw, .area = area, .motion = motion};
    pthread_mutex_unlock(&g_lock);
}

bool coop_net_remote(CoopRemote *out) {
    if (!out) return false;
    pthread_mutex_lock(&g_lock);
    *out = g_remote;
    pthread_mutex_unlock(&g_lock);
    return out->present;
}

void coop_net_emote(uint8_t emote) {
    if (!g_configured || emote > 15) return;
    pthread_mutex_lock(&g_lock);
    g_pending_emote = emote;
    g_has_pending_emote = true;
    pthread_mutex_unlock(&g_lock);
}

RT_ON_RESET(coop_net_on_reset) { coop_net_reset(); }
