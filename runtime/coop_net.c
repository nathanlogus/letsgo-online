// Native client for the small social co-op relay (protocol in docs/COOP-ONLINE.md).
// The worker never touches game memory. Game hooks publish a copied pose; rendering reads a copied peer pose.
#include "coop_net.h"
#include "coop_battle_link.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hle.h"
#include "rt.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET CoopSocket;
#define COOP_INVALID_SOCKET INVALID_SOCKET
#define coop_close closesocket
#define coop_shutdown(s) shutdown((s), SD_BOTH)
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int CoopSocket;
#define COOP_INVALID_SOCKET (-1)
#define coop_close close
#define coop_shutdown(s) shutdown((s), SHUT_RDWR)
#endif

#define COOP_MAX_FRAME 256
#define COOP_SNAPSHOT_NS 16666667ull  // 60 Hz, scheduled independently of incoming traffic
#define COOP_STICK_STALE_MS 250
#define COOP_JOIN_SETTLE_MS 300
#define COOP_PING_MS 1000

typedef struct {
    bool valid;
    float x, y, z, yaw;
    uint32_t area;
    uint8_t motion;
    int16_t stick_x, stick_y;
    uint64_t published_ns;
} LocalPose;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_thread;
static bool g_thread_started;
static _Atomic bool g_configured;
static _Atomic bool g_stop;
static CoopSocket g_socket = COOP_INVALID_SOCKET;
static LocalPose g_local;
static CoopRemote g_remote;
static CoopNetStats g_stats = {.relay_rtt_ms = -1};
static CoopAppearance g_appearance;
static bool g_appearance_pending;
static uint64_t g_join_ready_since;
static struct { bool visible; float x, y, z; uint64_t updated_ns; } g_avatar_head;
static uint8_t g_pending_emote;
static uint8_t g_local_emote;
static CoopTeamUp g_team;
static uint8_t g_team_command=255;
static uint32_t g_team_command_id;
static uint64_t g_local_emote_at;
static bool g_has_pending_emote;
static char g_room_code[17];
static uint32_t g_sequence;
static char g_local_name[25] = "Trainer";
static uint8_t g_local_slot = 0xff;
static CoopOptions g_options;
static CoopSession g_session;
static uint8_t g_identity[16], g_token[16];
static bool g_options_loaded, g_request, g_rejected;
static unsigned g_generation;
static int g_control = -1;
static pthread_cond_t g_wake = PTHREAD_COND_INITIALIZER;

static void options_path(char path[1200]) {
    const char *save = getenv("LARECOMP_SAVE_DIR");
    snprintf(path, 1200, "%s/coop.ini", save && *save ? save : ".");
}
static void save_options(void) { // g_lock held; no save-game data is read or written
    char path[1200]; options_path(path);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "server=%s\nname=%s\n", g_options.server, g_options.name);
    fprintf(f, "identity=");
    for (unsigned i = 0; i < 16; i++) fprintf(f, "%02x", g_identity[i]);
    fprintf(f, "\n");
    for (unsigned i = 0; i < 4; i++) fprintf(f, "room%u=%s\n", i, g_options.saved_rooms[i]);
    fclose(f);
}
static void load_options(void) { // g_lock held
    if (g_options_loaded) return;
    g_options_loaded = true;
    snprintf(g_options.server, sizeof g_options.server, "127.0.0.1:7381");
    snprintf(g_options.name, sizeof g_options.name, "Trainer");
    char path[1200], line[512]; options_path(path);
    FILE *f = fopen(path, "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            char *v = strchr(line, '='); if (!v) continue; *v++ = 0;
            if (!strcmp(line, "server")) snprintf(g_options.server, sizeof g_options.server, "%s", v);
            if (!strcmp(line, "name")) snprintf(g_options.name, sizeof g_options.name, "%s", v);
            if (!strcmp(line, "identity") && strlen(v) == 32)
                for (unsigned i = 0, b; i < 16; i++) if (sscanf(v + i*2, "%2x", &b) == 1) g_identity[i] = b;
            if (strlen(line) == 5 && !strncmp(line, "room", 4) && line[4] >= '0' && line[4] <= '3')
                snprintf(g_options.saved_rooms[line[4]-'0'], 17, "%s", v);
        }
        fclose(f);
    }
    bool empty = true; for (unsigned i = 0; i < 16; i++) empty &= !g_identity[i];
    if (empty) {
#ifdef _WIN32
        for (unsigned i = 0; i < 16; i += 4) { unsigned int n; if (rand_s(&n)) n = (unsigned int)(plat_now_ns() >> i); memcpy(g_identity+i, &n, 4); }
#else
        f = fopen("/dev/urandom", "rb");
        if (f) { size_t n = fread(g_identity, 1, 16, f); (void)n; fclose(f); }
#endif
        save_options();
    }
}

void coop_net_options(CoopOptions *out) {
    if (!out) return;
    pthread_mutex_lock(&g_lock); load_options(); *out = g_options; pthread_mutex_unlock(&g_lock);
}
void coop_net_preferences(const CoopOptions *options) {
    if (!options || !options->server[0] || !options->name[0]) return;
    pthread_mutex_lock(&g_lock); load_options();
    snprintf(g_options.server,sizeof g_options.server,"%s",options->server);
    snprintf(g_options.name,sizeof g_options.name,"%s",options->name);
    for(char *p=g_options.server;*p;p++) if((unsigned char)*p<32) *p=' ';
    for(char *p=g_options.name;*p;p++) if((unsigned char)*p<32) *p=' ';
    save_options(); pthread_mutex_unlock(&g_lock);
}
void coop_net_session(CoopSession *out) {
    if (!out) return;
    pthread_mutex_lock(&g_lock);
    *out = g_session; out->local_slot = g_local_slot;
    memcpy(out->room, g_room_code, sizeof out->room);
    memcpy(out->name, g_local_name, sizeof out->name);
    uint64_t now = plat_now_ns();
    out->snapshot_age_ms = g_remote.received_ns && now >= g_remote.received_ns ? (now - g_remote.received_ns)/1000000.0f : -1;
    pthread_mutex_unlock(&g_lock);
}
void coop_net_connect(const CoopOptions *options) {
    if (!options || !options->server[0] || !options->name[0]) return;
    pthread_mutex_lock(&g_lock); load_options();
    char saved_rooms[4][17]; memcpy(saved_rooms, g_options.saved_rooms, sizeof saved_rooms);
    g_options = *options;
    memcpy(g_options.saved_rooms, saved_rooms, sizeof saved_rooms);
    g_options.server[255] = g_options.name[24] = g_options.room[16] = 0;
    for (char *p = g_options.room; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
    for (char *p = g_options.server; *p; p++) if ((unsigned char)*p < 32) *p = ' ';
    for (char *p = g_options.name; *p; p++) if ((unsigned char)*p < 32) *p = ' ';
    g_request = true; g_generation++; g_configured = true;
    g_session.connection = COOP_CONNECTING; g_session.detail[0] = 0;
    save_options(); pthread_cond_signal(&g_wake); pthread_mutex_unlock(&g_lock);
}
void coop_net_leave(void) {
    coop_battle_link_reset();
    pthread_mutex_lock(&g_lock);
    g_request = false; g_generation++; g_configured = false;
    g_session.connection = COOP_OFFLINE; g_session.detail[0] = 0;
    memset(&g_remote, 0, sizeof g_remote); g_avatar_head.visible = false;
    memset(&g_team,0,sizeof g_team);g_team_command=255;
    pthread_cond_signal(&g_wake); pthread_mutex_unlock(&g_lock);
}
void coop_net_lock(bool locked) { pthread_mutex_lock(&g_lock); if (g_local_slot == g_session.host_slot && g_session.connection == COOP_CONNECTED) g_control = locked ? 1 : 0; pthread_mutex_unlock(&g_lock); }
void coop_net_remove_peer(void) { pthread_mutex_lock(&g_lock); if (g_local_slot == g_session.host_slot && g_session.connection == COOP_CONNECTED) g_control = 2; pthread_mutex_unlock(&g_lock); }
void coop_net_forget_room(unsigned i) { if (i >= 4) return; pthread_mutex_lock(&g_lock); g_options.saved_rooms[i][0] = 0; save_options(); pthread_mutex_unlock(&g_lock); }
void coop_net_activity(CoopActivity activity) { pthread_mutex_lock(&g_lock); g_session.activity = activity; pthread_mutex_unlock(&g_lock); }

static uint16_t get_u16(const uint8_t *p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
static int16_t get_i16(const uint8_t *p) { return (int16_t)get_u16(p); }
static uint32_t get_u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t get_u64(const uint8_t *p) { return (uint64_t)get_u32(p) | (uint64_t)get_u32(p + 4) << 32; }
static void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_i16(uint8_t *p, int16_t v) { put_u16(p, (uint16_t)v); }
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
        int no_delay = 1;
        if (setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&no_delay, sizeof no_delay) != 0)
            LOG("[coop] couldn't disable TCP packet batching\n");
#ifdef _WIN32
        u_long nonblock = 1;
        ioctlsocket(s, FIONBIO, &nonblock);
#else
        int flags = fcntl(s, F_GETFL, 0); fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
        int connected = connect(s, a->ai_addr, (int)a->ai_addrlen);
        if (connected != 0) {
            fd_set writes, errors; FD_ZERO(&writes); FD_ZERO(&errors); FD_SET(s, &writes); FD_SET(s, &errors);
            struct timeval timeout = {2, 0};
            int selected = select((int)s + 1, NULL, &writes, &errors, &timeout);
            int error = 1;
#ifdef _WIN32
            int error_size = sizeof error;
#else
            socklen_t error_size = sizeof error;
#endif
            if (selected > 0 && FD_ISSET(s, &writes) && !FD_ISSET(s, &errors) &&
                !getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&error, &error_size) && !error) connected = 0;
        }
#ifdef _WIN32
        nonblock = 0; ioctlsocket(s, FIONBIO, &nonblock);
        DWORD timeout_ms = 3000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms, sizeof timeout_ms);
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout_ms, sizeof timeout_ms);
#else
        fcntl(s, F_SETFL, flags);
        struct timeval io_timeout = {3, 0};
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &io_timeout, sizeof io_timeout);
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &io_timeout, sizeof io_timeout);
#endif
        if (connected == 0) { result = s; break; }
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
    coop_battle_link_reset();
    pthread_mutex_lock(&g_lock);
    memset(&g_remote, 0, sizeof g_remote);
    memset(&g_team,0,sizeof g_team);g_team_command=255;
    g_stats.snapshot_interval_ms = 0;
    g_stats.snapshots_received = 0;
    g_join_ready_since = 0;
    g_avatar_head.visible = false;
    pthread_mutex_unlock(&g_lock);
}

static void process_message(const uint8_t *p, size_t n) {
    if (!n) return;
    if(p[0]=='B'){coop_battle_link_message(p+1,n-1);return;}
    if(p[0]=='U' && n==12 && p[1]<=2 && (p[2]<=1 || p[2]==255) && p[3]<=7) {
        pthread_mutex_lock(&g_lock);
        g_team=(CoopTeamUp){.state=p[1],.from_slot=p[2],.reason=p[3],.id=get_u32(p+4),
            .expires_ns=p[1]==1 ? plat_now_ns()+(uint64_t)get_u32(p+8)*1000000ull : 0};
        pthread_mutex_unlock(&g_lock);
        LOG("[coop] team-up state %u, reason %u\n",p[1],p[3]);return;
    }
    if (p[0] == 'q' && n == 5) {
        // The echoed 32-bit monotonic millisecond counter also works across its wrap.
        uint32_t elapsed = (uint32_t)(plat_now_ns() / 1000000ull) - get_u32(p + 1);
        pthread_mutex_lock(&g_lock);
        g_stats.relay_rtt_ms = (float)elapsed;
        pthread_mutex_unlock(&g_lock);
        coop_battle_link_latency(elapsed);
        return;
    }
    if (p[0] == 'A' && n >= 3) {
        uint8_t slot = p[1], nc = p[2];
        if (slot > 1 || nc > 16 || (size_t)nc + 21 != n) return;
        pthread_mutex_lock(&g_lock);
        g_local_slot = slot;
        memcpy(g_room_code, p + 3, nc);
        g_room_code[nc] = 0;
        g_session.host_slot = p[3+nc]; g_session.locked = p[4+nc] != 0;
        memcpy(g_token, p + 5 + nc, 16);
        g_session.connection = COOP_CONNECTED;
        snprintf(g_session.detail, sizeof g_session.detail, "Connected — independent saves");
        bool saved = false;
        for (unsigned i = 0; i < 4; i++) saved |= !strcmp(g_options.saved_rooms[i], g_room_code);
        if (!saved) {
            for (unsigned i = 3; i > 0; i--) memcpy(g_options.saved_rooms[i], g_options.saved_rooms[i-1], 17);
            memcpy(g_options.saved_rooms[0], g_room_code, 17);
            save_options();
        }
        pthread_mutex_unlock(&g_lock);
        LOG("[coop] joined room %s as player %u\n", g_room_code, slot + 1);
        return;
    }
    if (p[0] == 'P' && n >= 3) {
        uint8_t slot = p[1];
        if (slot > 1 || (size_t)p[2] + 19 != n || p[2] > 48) return;
        pthread_mutex_lock(&g_lock);
        memset(&g_remote, 0, sizeof g_remote);
        g_remote.present = true;
        g_remote.slot = slot;
        g_remote.emote = 0;
        g_remote.emote_received_ns = 0;
        g_appearance_pending = true;
        size_t len = p[2] < sizeof g_remote.name - 1 ? p[2] : sizeof g_remote.name - 1;
        memcpy(g_remote.name, p + 3, len);
        g_remote.name[len] = 0;
        memcpy(g_remote.identity, p + 3 + p[2], 16);
        pthread_mutex_unlock(&g_lock);
        LOG("[coop] %s joined the room\n", g_remote.name);
        return;
    }
    if (p[0] == 'L' && n == 2) {
        clear_remote();
        LOG("[coop] the other player left\n");
        return;
    }
    if (p[0] == 'H' && n == 3 && p[1] <= 1 && p[2] <= 1) {
        pthread_mutex_lock(&g_lock); g_session.host_slot = p[1]; g_session.locked = p[2]; pthread_mutex_unlock(&g_lock); return;
    }
    if (p[0] == 'S' && n == 30 && p[29] <= COOP_STORY) {  // slot + 28-byte snapshot
        pthread_mutex_lock(&g_lock);
        if (g_remote.present && p[1] == g_remote.slot) {
            const uint8_t *b = p + 2;
            g_remote.area = get_u32(b);
            g_remote.sequence = get_u32(b + 4);
            g_remote.x = (float)(int32_t)get_u32(b + 8);
            g_remote.y = (float)(int32_t)get_u32(b + 12);
            g_remote.z = (float)(int32_t)get_u32(b + 16);
            g_remote.yaw = (float)get_u16(b + 20) * (2.0f * 3.14159265358979323846f / 65535.0f);
            g_remote.world_ready = (b[22] & 0x80) != 0;
            g_remote.motion = b[22] & 0x7f;
            g_remote.stick_x = get_i16(b + 23);
            g_remote.stick_y = get_i16(b + 25);
            g_remote.activity = b[27];
            uint64_t now = plat_now_ns();
            if (g_remote.received_ns && now >= g_remote.received_ns) {
                float interval = (float)(now - g_remote.received_ns) / 1000000.0f;
                g_stats.snapshot_interval_ms = g_stats.snapshot_interval_ms > 0 ?
                    g_stats.snapshot_interval_ms * 0.9f + interval * 0.1f : interval;
            }
            g_stats.snapshots_received++;
            g_remote.received_ns = now;
        }
        pthread_mutex_unlock(&g_lock);
        return;
    }
    if (p[0] == 'T' && n == 84 && p[2] <= 1 && p[3] <= 10) {
        pthread_mutex_lock(&g_lock);
        if (g_remote.present && p[1] == g_remote.slot) {
            g_remote.appearance.valid = true;
            g_remote.appearance.gender = p[2];
            g_remote.appearance.style = p[3];
            memcpy(g_remote.appearance.parts, p + 4, COOP_APPEARANCE_BYTES);
            LOG("[coop] received peer appearance: gender %u, style %u\n", p[2], p[3]);
        }
        pthread_mutex_unlock(&g_lock);
        return;
    }
    if (p[0] == 'E' && n == 3) {
        pthread_mutex_lock(&g_lock);
        if (g_remote.present && p[1] == g_remote.slot && p[2] <= 15) {
            g_remote.emote = p[2];
            g_remote.emote_received_ns = plat_now_ns();
        }
        pthread_mutex_unlock(&g_lock);
        LOG("[coop] player %u sent emote %u\n", p[1] + 1, p[2]);
        return;
    }
    if (p[0] == 'X') {
        size_t len = n >= 2 && p[1] <= n - 2 ? p[1] : n - 1;
        char why[100];
        if (len >= sizeof why) len = sizeof why - 1;
        memcpy(why, p + (n >= 2 ? 2 : 1), len);
        why[len] = 0;
        pthread_mutex_lock(&g_lock);
        g_rejected = true; g_session.connection = COOP_ERROR;
        snprintf(g_session.detail, sizeof g_session.detail, "%s", why);
        pthread_mutex_unlock(&g_lock);
        LOG("[coop] relay refused connection: %s\n", why);
    }
}

static bool current_request(unsigned generation) {
    pthread_mutex_lock(&g_lock);
    bool current = g_request && generation == g_generation;
    pthread_mutex_unlock(&g_lock);
    return current && !atomic_load(&g_stop);
}

static void run_connection(const CoopOptions *options, uint64_t build_id, unsigned generation, bool resume) {
    char host[256], port[16];
    if (!parse_server(options->server, host, sizeof host, port, sizeof port)) return;
    const char *name = options->name;
    size_t name_len = strlen(name);
    if (name_len > 48) name_len = 48;
    bool create = options->host && !resume;
    const char *room = resume ? g_room_code : options->room;
    size_t room_len = room ? strlen(room) : 0;
    if (!create && (!room_len || room_len > 16)) {
        LOG("[coop] join mode needs LARECOMP_COOP_ROOM\n");
        return;
    }

    LOG("[coop] connecting to %s:%s\n", host, port);
    CoopSocket s = connect_to(host, port);
    if (s == COOP_INVALID_SOCKET) { LOG("[coop] couldn't connect to relay\n"); return; }
    set_socket(s);
    uint8_t hello[128];
    size_t n = 0;
    hello[n++] = resume ? 'R' : create ? 'C' : 'J';
    hello[n++] = 5;  // social sessions and voluntary team-up invitations
    put_u64(hello + n, g_program_id); n += 8;
    put_u64(hello + n, build_id); n += 8;
    if (!create) {
        hello[n++] = (uint8_t)room_len;
        memcpy(hello + n, room, room_len); n += room_len;
    }
    hello[n++] = (uint8_t)name_len;
    memcpy(hello + n, name, name_len); n += name_len;
    memcpy(hello + n, g_identity, 16); n += 16;
    memcpy(hello + n, resume ? g_token : (uint8_t[16]){0}, 16); n += 16;
    if (!send_frame(s, hello[0], hello + 1, n - 1)) { LOG("[coop] couldn't send room request\n"); goto close_socket; }

    uint64_t last_ping = 0, next_snapshot = plat_now_ns(), last_reply = next_snapshot, started = next_snapshot;
    while (current_request(generation) && !g_rejected) {
        uint64_t now = plat_now_ns();
        if (now - last_ping >= COOP_PING_MS * 1000000ull) {
            uint8_t ping[4]; put_u32(ping, (uint32_t)(now / 1000000ull));
            if (!send_frame(s, 'Q', ping, sizeof ping)) break;
            last_ping = now;
        }

        LocalPose pose;
        CoopAppearance appearance;
        bool send_appearance;
        uint8_t emote = 0;
        bool send_emote = false;
        uint8_t team_command=255; uint32_t team_id=0;
        CoopActivity activity;
        int control;
        pthread_mutex_lock(&g_lock);
        pose = g_local;
        activity = g_session.activity;
        control = g_control; g_control = -1;
        appearance = g_appearance;
        send_appearance = g_appearance_pending && appearance.valid;
        if (send_appearance) g_appearance_pending = false;
        if (g_has_pending_emote) { emote = g_pending_emote; g_has_pending_emote = false; send_emote = true; }
        // Invitations follow a fresh snapshot in the same TCP stream, so leaving
        // Communicate cannot race the relay's last busy-state snapshot.
        if(g_team_command<=3 && (g_team_command>=2 || now>=next_snapshot)) {
            team_command=g_team_command;team_id=g_team_command_id;g_team_command=255;
        }
        pthread_mutex_unlock(&g_lock);
        if (send_appearance) {
            uint8_t body[2 + COOP_APPEARANCE_BYTES];
            body[0] = appearance.gender; body[1] = appearance.style;
            memcpy(body + 2, appearance.parts, COOP_APPEARANCE_BYTES);
            if (!send_frame(s, 'T', body, sizeof body)) break;
        }
        if (now >= next_snapshot) {
            bool ready = activity == COOP_OVERWORLD && pose.valid && now >= pose.published_ns &&
                         now - pose.published_ns <= COOP_STICK_STALE_MS * 1000000ull;
            uint8_t body[28];
            put_u32(body, pose.area);
            put_u32(body + 4, ++g_sequence);
            put_u32(body + 8, (uint32_t)quantize_cm(pose.x));
            put_u32(body + 12, (uint32_t)quantize_cm(pose.y));
            put_u32(body + 16, (uint32_t)quantize_cm(pose.z));
            put_u16(body + 20, quantize_yaw(pose.yaw));
            body[22] = (pose.motion & 0x7f) | (ready ? 0x80 : 0);
            put_i16(body + 23, ready ? pose.stick_x : 0);
            put_i16(body + 25, ready ? pose.stick_y : 0);
            body[27] = activity;
            if (!send_frame(s, 'S', body, sizeof body)) break;
            next_snapshot += COOP_SNAPSHOT_NS;
            // Never burst old poses to catch up after a stalled connection.
            if (next_snapshot <= now) next_snapshot = now + COOP_SNAPSHOT_NS;
        }
        if (send_emote && !send_frame(s, 'E', &emote, 1)) break;
        if (team_command<=3) {
            uint8_t body[5]={team_command};put_u32(body+1,team_id);
            if(!send_frame(s,'U',body,sizeof body))break;
        }
        coop_battle_link_tick();
        { uint8_t body[255];size_t size;bool failed=false;
          while(coop_battle_link_pop(body,&size))if(!send_frame(s,'B',body,size)){failed=true;break;}
          if(failed)break;
        }
        if (control >= 0) { uint8_t value = control == 2 ? (uint8_t)(1 - g_local_slot) : control; if (!send_frame(s, control == 2 ? 'D' : 'K', &value, 1)) break; }
        if (now - last_reply > 5000000000ull) break;
        if (g_local_slot > 1 && now - started > 10000000000ull) break;

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(s, &readfds);
        // Incoming traffic must not decide the send cadence. Wake for the next pose
        // deadline instead of adding a fixed 50 ms receive wait to each update.
        uint64_t after_send = plat_now_ns();
        uint64_t wait_us = next_snapshot > after_send ? (next_snapshot - after_send + 999) / 1000 : 0;
        struct timeval timeout = {0, (long)wait_us};
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
        last_reply = plat_now_ns();
        process_message(packet, got);
    }
    if (!atomic_load(&g_stop) && !current_request(generation)) send_frame(s, 'L', NULL, 0);
close_socket:
    set_socket(COOP_INVALID_SOCKET);
    coop_shutdown(s);
    coop_close(s);
    clear_remote();
}

static void *coop_worker(void *unused) {
    (void)unused;
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return NULL;
#endif
    uint64_t build_id = 0;
    bool compatible = main_build_id(&build_id) && build_id == 0x80ad7de6210d3f0aull && g_program_id == 0x0100187003a36000ull;
    while (!atomic_load(&g_stop)) {
        pthread_mutex_lock(&g_lock);
        while (!g_request && !atomic_load(&g_stop)) pthread_cond_wait(&g_wake, &g_lock);
        CoopOptions options = g_options;
        unsigned generation = g_generation;
        g_rejected = false; g_local_slot = 0xff; g_control = -1; g_room_code[0] = 0;
        memset(g_token, 0, sizeof g_token);
        snprintf(g_local_name, sizeof g_local_name, "%s", options.name);
        pthread_mutex_unlock(&g_lock);
        if (atomic_load(&g_stop)) break;
        if (!compatible) {
            pthread_mutex_lock(&g_lock);
            g_session.connection = COOP_ERROR;
            snprintf(g_session.detail, sizeof g_session.detail, "Online avatars currently require Let's Go Eevee 1.0.0. Check your extracted game files.");
            if (generation == g_generation) g_request = false;
            pthread_mutex_unlock(&g_lock);
            continue;
        }
        bool resume = false;
        for (unsigned attempt = 1; attempt <= 3 && current_request(generation); attempt++) {
            pthread_mutex_lock(&g_lock);
            g_session.connection = resume ? COOP_RECONNECTING : COOP_CONNECTING;
            g_session.attempt = attempt;
            g_stats.relay_rtt_ms = -1; g_appearance_pending = g_appearance.valid;
            pthread_mutex_unlock(&g_lock);
            run_connection(&options, build_id, generation, resume);
            pthread_mutex_lock(&g_lock);
            bool accepted = g_local_slot <= 1;
            resume |= accepted;
            if (accepted) { attempt = 0; g_local_slot = 0xff; }
            bool rejected = g_rejected;
            if (!rejected && generation == g_generation && g_request) g_session.connection = COOP_RECONNECTING;
            pthread_mutex_unlock(&g_lock);
            if (rejected || !current_request(generation)) break;
            // A bounded reconnect cycle; a successful session resets the retry budget.
            pthread_mutex_lock(&g_lock);
            struct timespec until; timespec_get(&until, TIME_UTC); until.tv_sec++;
            pthread_mutex_unlock(&g_lock);
            pthread_mutex_lock(&g_lock);
            if (generation == g_generation && g_request) pthread_cond_timedwait(&g_wake, &g_lock, &until);
            pthread_mutex_unlock(&g_lock);
        }
        pthread_mutex_lock(&g_lock);
        if (generation == g_generation && g_request) {
            g_request = false;
            if (!g_rejected) { g_session.connection = COOP_ERROR; snprintf(g_session.detail, sizeof g_session.detail, "Connection lost after 3 attempts. Retry or leave to play solo."); }
        }
        pthread_mutex_unlock(&g_lock);
    }
#ifdef _WIN32
    WSACleanup();
#endif
    LOG("[coop] network worker stopped\n");
    return NULL;
}

void coop_net_start(void) {
    if (g_thread_started) return;
    const char *server = getenv("LARECOMP_COOP_SERVER");
    atomic_store(&g_stop, false);
    pthread_mutex_lock(&g_lock);
    load_options();
    if (server && *server) {
        snprintf(g_options.server, sizeof g_options.server, "%s", server);
        const char *name = getenv("LARECOMP_COOP_NAME"), *room = getenv("LARECOMP_COOP_ROOM"), *host = getenv("LARECOMP_COOP_HOST");
        if (name && *name) snprintf(g_options.name, sizeof g_options.name, "%s", name);
        snprintf(g_options.room, sizeof g_options.room, "%s", room ? room : "");
        g_options.host = host && !strcmp(host, "1");
        g_request = true;
    }
    g_configured = g_request;
    snprintf(g_local_name, sizeof g_local_name, "%s", g_options.name);
    memset(&g_local, 0, sizeof g_local);
    g_local_slot = 0xff;
    memset(&g_remote, 0, sizeof g_remote);
    g_stats = (CoopNetStats){.relay_rtt_ms = -1};
    memset(&g_appearance, 0, sizeof g_appearance);
    memset(&g_avatar_head, 0, sizeof g_avatar_head);
    g_appearance_pending = false;
    g_join_ready_since = 0;
    g_has_pending_emote = false;
    memset(&g_team,0,sizeof g_team);g_team_command=255;
    g_local_emote = 0; g_local_emote_at = 0;
    pthread_mutex_unlock(&g_lock);
    if (pthread_create(&g_thread, NULL, coop_worker, NULL)) {
        g_configured = false;
        pthread_mutex_lock(&g_lock);
        g_session.connection = COOP_ERROR;
        snprintf(g_session.detail, sizeof g_session.detail, "Couldn't start the online worker. Restart the game to retry.");
        pthread_mutex_unlock(&g_lock);
        LOG("[coop] couldn't start the network worker\n");
        return;
    }
    g_thread_started = true;
}

bool coop_net_enabled(void) { return g_configured; }

void coop_net_stats(CoopNetStats *out) {
    if (!out) return;
    pthread_mutex_lock(&g_lock);
    *out = g_stats;
    pthread_mutex_unlock(&g_lock);
}

void coop_net_reset(void) {
    if (!g_thread_started) { g_configured = false; return; }
    atomic_store(&g_stop, true);
    pthread_mutex_lock(&g_lock);
    g_request = false; g_generation++; pthread_cond_signal(&g_wake);
    CoopSocket s = g_socket;
    pthread_mutex_unlock(&g_lock);
    if (s != COOP_INVALID_SOCKET) coop_shutdown(s);
    pthread_join(g_thread, NULL);
    g_thread_started = false;
    g_configured = false;
    pthread_mutex_lock(&g_lock);
    g_socket = COOP_INVALID_SOCKET;
    memset(&g_local, 0, sizeof g_local);
    g_local_emote = 0; g_local_emote_at = 0;
    g_local_slot = 0xff;
    memset(&g_team,0,sizeof g_team);g_team_command=255;
    memset(&g_remote, 0, sizeof g_remote);
    g_stats = (CoopNetStats){.relay_rtt_ms = -1};
    memset(&g_appearance, 0, sizeof g_appearance);
    memset(&g_avatar_head, 0, sizeof g_avatar_head);
    g_appearance_pending = false;
    g_join_ready_since = 0;
    g_room_code[0] = 0;
    pthread_mutex_unlock(&g_lock);
}

void coop_net_publish_position(float x, float y, float z, float yaw, uint32_t area, uint8_t motion, int16_t stick_x, int16_t stick_y) {
    pthread_mutex_lock(&g_lock);
    g_local = (LocalPose){.valid = true, .x = x, .y = y, .z = z, .yaw = yaw, .area = area, .motion = motion, .stick_x = stick_x, .stick_y = stick_y, .published_ns = plat_now_ns()};
    pthread_mutex_unlock(&g_lock);
}

static bool poses_ready(uint64_t now) { // g_lock held
    return g_local.valid && g_appearance.valid && g_remote.present && g_remote.world_ready &&
           g_remote.appearance.valid && g_local.area == g_remote.area &&
           now >= g_local.published_ns && now - g_local.published_ns <= COOP_STICK_STALE_MS * 1000000ull &&
           now >= g_remote.received_ns && now - g_remote.received_ns <= COOP_STICK_STALE_MS * 1000000ull;
}
static bool worlds_ready(uint64_t now) { // g_lock held
    return g_session.activity == COOP_OVERWORLD && g_remote.activity == COOP_OVERWORLD && poses_ready(now);
}

void coop_net_publish_appearance(const CoopAppearance *appearance) {
    if (!g_configured || !appearance || !appearance->valid) return;
    pthread_mutex_lock(&g_lock);
    if (memcmp(&g_appearance, appearance, sizeof *appearance)) {
        g_appearance = *appearance;
        g_appearance_pending = true;
        LOG("[coop] local appearance: gender %u, style %u\n", appearance->gender, appearance->style);
    }
    pthread_mutex_unlock(&g_lock);
}

bool coop_net_avatar_ready(void) {
    pthread_mutex_lock(&g_lock);
    uint64_t now = plat_now_ns();
    if (!worlds_ready(now)) g_join_ready_since = 0;
    else if (!g_join_ready_since) g_join_ready_since = now;
    bool ready = g_join_ready_since && now - g_join_ready_since >= COOP_JOIN_SETTLE_MS * 1000000ull;
    pthread_mutex_unlock(&g_lock);
    return ready;
}

void coop_net_set_avatar_head(bool visible, float x, float y, float z) {
    pthread_mutex_lock(&g_lock);
    g_avatar_head.visible = visible;
    g_avatar_head.x = x; g_avatar_head.y = y; g_avatar_head.z = z;
    g_avatar_head.updated_ns = plat_now_ns();
    pthread_mutex_unlock(&g_lock);
}

bool coop_net_avatar_head(float *x, float *y, float *z) {
    if (!x || !y || !z) return false;
    pthread_mutex_lock(&g_lock);
    uint64_t now = plat_now_ns();
    bool visible = worlds_ready(now) && g_avatar_head.visible && now >= g_avatar_head.updated_ns &&
                   now - g_avatar_head.updated_ns <= COOP_STICK_STALE_MS * 1000000ull;
    *x = g_avatar_head.x; *y = g_avatar_head.y; *z = g_avatar_head.z;
    pthread_mutex_unlock(&g_lock);
    return visible;
}

bool coop_net_local_head(float *x, float *y, float *z, char name[25]) {
    if (!x || !y || !z || !name) return false;
    pthread_mutex_lock(&g_lock);
    bool visible = worlds_ready(plat_now_ns());
    *x = g_local.x; *y = g_local.y + 150; *z = g_local.z;
    memcpy(name, g_local_name, sizeof g_local_name);
    pthread_mutex_unlock(&g_lock);
    return visible;
}

uint8_t coop_net_local_slot(void) {
    pthread_mutex_lock(&g_lock);
    uint8_t slot = g_local_slot;
    pthread_mutex_unlock(&g_lock);
    return slot;
}

bool coop_net_remote_stick(int16_t *x, int16_t *y) {
    if (!x || !y) return false;
    pthread_mutex_lock(&g_lock);
    uint64_t now = plat_now_ns();
    bool fresh = worlds_ready(now);
    *x = fresh ? g_remote.stick_x : 0;
    *y = fresh ? g_remote.stick_y : 0;
    pthread_mutex_unlock(&g_lock);
    return fresh;
}

bool coop_net_remote(CoopRemote *out) {
    if (!out) return false;
    pthread_mutex_lock(&g_lock);
    *out = g_remote;
    pthread_mutex_unlock(&g_lock);
    return out->present;
}

bool coop_net_remote_near(float radius_cm) {
    if (radius_cm <= 0) return false;
    pthread_mutex_lock(&g_lock);
    bool is_near = worlds_ready(plat_now_ns());
    if (is_near) {
        float dx = g_local.x - g_remote.x, dy = g_local.y - g_remote.y, dz = g_local.z - g_remote.z;
        is_near = dx * dx + dy * dy + dz * dz <= radius_cm * radius_cm;
    }
    pthread_mutex_unlock(&g_lock);
    return is_near;
}

void coop_net_emote(uint8_t emote) {
    if (!emote || emote > 15) return;
    pthread_mutex_lock(&g_lock);
    uint64_t now = plat_now_ns();
    if (g_local_emote_at && now >= g_local_emote_at && now - g_local_emote_at < 400000000ull) {
        pthread_mutex_unlock(&g_lock); return;
    }
    g_local_emote = emote; g_local_emote_at = now;
    if (g_configured) { g_pending_emote = emote; g_has_pending_emote = true; }
    pthread_mutex_unlock(&g_lock);
    LOG("[coop] local emote %u\n", emote);
}

void coop_net_teamup(CoopTeamUp *out) {
    if(!out)return;
    pthread_mutex_lock(&g_lock);*out=g_team;pthread_mutex_unlock(&g_lock);
}
bool coop_net_teamup_action(uint8_t command) {
    pthread_mutex_lock(&g_lock);
    bool ok=g_session.connection==COOP_CONNECTED && g_remote.present && command<=3 &&
        (command==0 ? g_team.state==0 : command==1 || command==2 ?
         g_team.state==1 && g_team.from_slot!=g_local_slot :
         g_team.state==2 || (g_team.state==1 && g_team.from_slot==g_local_slot));
    if(ok){g_team_command=command;g_team_command_id=g_team.id;g_team.reason=0;}
    pthread_mutex_unlock(&g_lock);return ok;
}

bool coop_net_local_emote(uint8_t *emote, uint64_t *at, float *x, float *y, float *z) {
    if (!emote || !at || !x || !y || !z) return false;
    pthread_mutex_lock(&g_lock);
    uint64_t now = plat_now_ns();
    bool visible = g_local.valid && g_local_emote_at && now >= g_local.published_ns &&
        now-g_local.published_ns <= COOP_STICK_STALE_MS*1000000ull &&
        now >= g_local_emote_at && now-g_local_emote_at < 3000000000ull;
    *emote=g_local_emote; *at=g_local_emote_at;
    *x=g_local.x; *y=g_local.y+150; *z=g_local.z;
    pthread_mutex_unlock(&g_lock); return visible;
}

bool coop_net_social_near(float radius_cm) {
    if (radius_cm <= 0) return false;
    pthread_mutex_lock(&g_lock);
    bool is_near = (g_session.activity == COOP_OVERWORLD || g_session.activity == COOP_MENU) &&
        g_remote.activity == COOP_OVERWORLD && poses_ready(plat_now_ns());
    if (is_near) {
        float dx=g_local.x-g_remote.x, dy=g_local.y-g_remote.y, dz=g_local.z-g_remote.z;
        is_near = dx*dx+dy*dy+dz*dz <= radius_cm*radius_cm;
    }
    pthread_mutex_unlock(&g_lock); return is_near;
}

RT_ON_RESET(coop_net_on_reset) { coop_net_reset(); }
