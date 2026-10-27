// bcat:u, the game's delivery cache (BCAT), served from a host folder (bcat_root), holding
// <folder>/<directory>/<file>. Mystery Gift's "Get via internet" syncs the cache and reads every file of every
// directory (docs/ACCOUNTS-MYSTERY-GIFTS-ROADMAP.md, native Mystery Gift). Without the variable the cache is empty.
// Shapes as in switchbrew: bcat:u, IBcatService, IDeliveryCacheProgressService, IDeliveryCacheStorageService,
// IDeliveryCacheFileService, IDeliveryCacheDirectoryService. Names are 0x20-byte fields.
#include <dirent.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../hle.h"
#include "../ipc.h"
void rt_call_original(Cpu *c, uint64_t addr);

#define BCAT_NOT_FOUND MAKE_RESULT(122, 2)

// LARECOMP_BCAT_DIR, else the profile's cards (<save>.bcat, written by the launcher: runtime/account_delivery.inc)
static const char *bcat_root(void) {
    extern const char *gift_bcat_dir(void);
    const char *r = getenv("LARECOMP_BCAT_DIR");
    return r && *r ? r : gift_bcat_dir();
}
bool bcat_online(void) { return bcat_root() != NULL; }  // (network.c: "Get via internet" connects only with cards to serve)

// A 0x20-byte name field as a C string; false for an empty or unsafe one (a path out of the folder).
static bool bcat_name(const uint8_t *field, char out[0x21]) {
    memcpy(out, field, 0x20);
    out[0x20] = 0;
    return out[0] && out[0] != '.' && !strpbrk(out, "/\\:");
}

static uint32_t bcat_event(void) {  // (signalled: the sync is already done when the game waits)
    uint32_t w, r;
    KEvent *ev = kernel_create_event_pair(&w, &r);
    pthread_mutex_lock(&g_klock);
    kernel_signal(ev);
    pthread_mutex_unlock(&g_klock);
    return r;
}

static void progress(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: resp->copy_handles[resp->ncopy++] = bcat_event(); break;  // GetEvent
    case 1: {  // GetImpl: DeliveryCacheProgressImpl (0x200), status Done (9), result 0
        IpcBuf *b = out_buf(req, 0);
        if (b->size >= 8) memset((void *)(uintptr_t)b->addr, 0, b->size), *(int32_t *)(uintptr_t)b->addr = 9;
        LOG("[bcat] progress: done (buffer %llu bytes)\n", (unsigned long long)b->size);
        break;
    }
    default: LOG("[bcat] IDeliveryCacheProgressService cmd %u (ok)\n", req->cmd);
    }
}

static void bcat_service(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 10100: case 10101:  // RequestSyncDeliveryCache(WithDirectoryName)
        LOG("[bcat] sync requested (cmd %u)\n", req->cmd);
        ipc_return_object(resp, service_new("IDeliveryCacheProgressService", progress, NULL));
        break;
    default: LOG("[bcat] IBcatService cmd %u (ok)\n", req->cmd);
    }
}

// A file's digest: the MD5 of its contents (as yuzu's BCAT backend); games may check it against the data they read.
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
static void bcat_digest(const char *path, uint8_t out[0x10]) {
    memset(out, 0, 0x10);
    int fd = open(path, O_RDONLY | O_BINARY);
    struct stat sb;
    if (fd < 0) return;
    uint8_t *buf = !fstat(fd, &sb) && sb.st_size < (64 << 20) ? malloc((size_t)sb.st_size + 1) : NULL;
    BCRYPT_ALG_HANDLE alg = NULL;
    if (buf && read(fd, buf, (size_t)sb.st_size) == sb.st_size && !BCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, NULL, 0))
        BCryptHash(alg, NULL, 0, buf, (ULONG)sb.st_size, out, 0x10);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    free(buf);
    close(fd);
}
#else
static void bcat_digest(const char *path, uint8_t out[0x10]) { (void)path; memset(out, 0, 0x10); }  // shortcut: no MD5 off Windows, add one if the probe moves to Linux
#endif

typedef struct { char path[1200]; int fd; } BcatFile;
typedef struct { char path[1200]; } BcatDir;

static void bcat_file_close(Service *s) {
    BcatFile *f = s->state;
    if (f) { if (f->fd >= 0) close(f->fd); free(f); s->state = NULL; }
}

static void file_service(Service *s, IpcReq *req, IpcResp *resp) {
    BcatFile *f = s->state;
    switch (req->cmd) {
    case 0: {  // Open(DirectoryName, FileName)
        char dir[0x21], name[0x21];
        if (f->fd >= 0) close(f->fd), f->fd = -1;
        if (!bcat_root() || !bcat_name(req->data, dir) || !bcat_name(req->data + 0x20, name)) { resp->result = BCAT_NOT_FOUND; break; }
        snprintf(f->path, sizeof f->path, "%s/%s/%s", bcat_root(), dir, name);
        f->fd = open(f->path, O_RDONLY);
        LOG("[bcat] open %s/%s: %s\n", dir, name, f->fd >= 0 ? "ok" : "not found");
        if (f->fd < 0) resp->result = BCAT_NOT_FOUND;
        break;
    }
    case 1: {  // Read(offset) -> size, B
        IpcBuf *b = out_buf(req, 0);
        ssize_t n = f->fd >= 0 ? pread(f->fd, (void *)(uintptr_t)b->addr, (size_t)b->size, IN(int64_t, 0)) : -1;
        LOG("[bcat] read %s at %lld: %lld bytes -> guest %llx\n", f->path, (long long)IN(int64_t, 0), (long long)n,
            (unsigned long long)b->addr);
        if (n < 0) { resp->result = BCAT_NOT_FOUND; break; }
#ifdef _WIN32
        // research: LARECOMP_BCAT_WATCH=<hex offsets>: who reads these words of the file once it's in the game's buffer
        const char *w = getenv("LARECOMP_BCAT_WATCH");
        extern void memwatch_watch(uint64_t addr);
        for (const char *p = w; p && *p;) {
            memwatch_watch(b->addr + strtoull(p, (char **)&p, 16));
            while (*p == ',') p++;
        }
#endif
        OUT(int64_t, 0) = n;
        resp_size(resp, 8);
        break;
    }
    case 2: {  // GetSize
        struct stat sb;
        if (f->fd < 0 || fstat(f->fd, &sb)) { resp->result = BCAT_NOT_FOUND; break; }
        OUT(int64_t, 0) = sb.st_size;
        resp_size(resp, 8);
        break;
    }
    case 3: bcat_digest(f->path, resp->data); resp_size(resp, 0x10); break;  // GetDigest
    default: LOG("[bcat] IDeliveryCacheFileService cmd %u (ok)\n", req->cmd);
    }
}

// The files of a directory (regular files with a valid name), at most max entries of 0x38 bytes (name, size, digest).
static int dir_entries(const char *path, uint8_t *out, int max) {
    DIR *d = opendir(path);
    if (!d) return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        char full[1500], name[0x21];
        struct stat sb;
        snprintf(full, sizeof full, "%s/%s", path, e->d_name);
        if (strlen(e->d_name) > 0x1f || !bcat_name((const uint8_t *)e->d_name, name) || stat(full, &sb) || !S_ISREG(sb.st_mode)) continue;
        if (out && n < max) {
            memset(out + n * 0x38, 0, 0x38);
            memcpy(out + n * 0x38, e->d_name, strlen(e->d_name));
            *(int64_t *)(out + n * 0x38 + 0x20) = sb.st_size;
            bcat_digest(full, out + n * 0x38 + 0x28);
        }
        n++;
    }
    closedir(d);
    return out && n > max ? max : n;
}

static void dir_service(Service *s, IpcReq *req, IpcResp *resp) {
    BcatDir *d = s->state;
    switch (req->cmd) {
    case 0: {  // Open(DirectoryName)
        char dir[0x21];
        if (!bcat_root() || !bcat_name(req->data, dir)) { resp->result = BCAT_NOT_FOUND; break; }
        snprintf(d->path, sizeof d->path, "%s/%s", bcat_root(), dir);
        int n = dir_entries(d->path, NULL, 0);
        LOG("[bcat] open directory %s: %d files\n", dir, n);
        if (n < 0) resp->result = BCAT_NOT_FOUND;
        break;
    }
    case 1: {  // Read -> count, B entries
        IpcBuf *b = out_buf(req, 0);
        int n = dir_entries(d->path, (uint8_t *)(uintptr_t)b->addr, (int)(b->size / 0x38));
        OUT(int32_t, 0) = n < 0 ? 0 : n;
        resp_size(resp, 4);
        break;
    }
    case 2: { int n = dir_entries(d->path, NULL, 0); OUT(int32_t, 0) = n < 0 ? 0 : n; resp_size(resp, 4); break; }  // GetCount
    default: LOG("[bcat] IDeliveryCacheDirectoryService cmd %u (ok)\n", req->cmd);
    }
}

static void storage_service(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: {  // CreateFileService
        BcatFile *f = calloc(1, sizeof *f);
        f->fd = -1;
        Service *fs = service_new("IDeliveryCacheFileService", file_service, f);
        fs->on_close = bcat_file_close;
        ipc_return_object(resp, fs);
        break;
    }
    case 1: ipc_return_object(resp, service_new("IDeliveryCacheDirectoryService", dir_service, calloc(1, sizeof(BcatDir)))); break;
    case 10: {  // EnumerateDeliveryCacheDirectory -> count, B names
        IpcBuf *b = out_buf(req, 0);
        int n = 0, max = (int)(b->size / 0x20);
        DIR *d = bcat_root() ? opendir(bcat_root()) : NULL;
        struct dirent *e;
        while (d && (e = readdir(d)) && n < max) {
            char full[1500], name[0x21];
            struct stat sb;
            snprintf(full, sizeof full, "%s/%s", bcat_root(), e->d_name);
            if (strlen(e->d_name) > 0x1f || !bcat_name((const uint8_t *)e->d_name, name) || stat(full, &sb) || !S_ISDIR(sb.st_mode)) continue;
            memset((void *)(uintptr_t)(b->addr + n * 0x20), 0, 0x20);
            memcpy((void *)(uintptr_t)(b->addr + n * 0x20), e->d_name, strlen(e->d_name));
            n++;
        }
        if (d) closedir(d);
        LOG("[bcat] %d directories\n", n);
        OUT(int32_t, 0) = n;
        resp_size(resp, 4);
        break;
    }
    default: LOG("[bcat] IDeliveryCacheStorageService cmd %u (ok)\n", req->cmd);
    }
}

// research: LARECOMP_BCAT_TRACE=1: the Mystery Gift job that applies the loaded directories (normal, then zone) and
// the per-container call, with the job's state and result code before / after (Eevee addresses).
#define U64(a) (*(volatile uint64_t *)(uintptr_t)(a))
#define U32(a) (*(volatile uint32_t *)(uintptr_t)(a))
static uint64_t g_trace_container;
static bool guest_ptr(uint64_t p) { return p >= 0x7000000000ull && p < 0x7800000000ull && !(p & 7); }
// research: a loaded file's copy (LARECOMP_BCAT_TRACE_MARK=<text>, as UTF-16 when it starts with "u:"): searched in the
// container's memory up to two pointers away, then every 16th word of the 0x310 bytes from 0x208 before it is watched.
static void bcat_trace_find(uint64_t c0) {
    const char *m = getenv("LARECOMP_BCAT_TRACE_MARK");
    if (!m) return;
    uint8_t mark[64];
    size_t ml = 0;
    bool wide = !strncmp(m, "u:", 2);
    for (const char *p = wide ? m + 2 : m; *p && ml < sizeof mark - 1; p++) { mark[ml++] = (uint8_t)*p; if (wide) mark[ml++] = 0; }
    uint64_t lvl[3][64], n[3] = {1, 0, 0};
    lvl[0][0] = c0;
    for (int d = 0; d < 3; d++)
        for (uint64_t k = 0; k < n[d]; k++) {
            uint8_t *b = (uint8_t *)(uintptr_t)lvl[d][k];
            for (size_t o = 0; o + ml <= 0x400; o += 2)
                if (!memcmp(b + o, mark, ml)) {
                    uint64_t a = lvl[d][k] + o, card = a - 0x20a;
                    LOG("[bcat-trace] marker at %llx (depth %d, +%zx): file at %llx\n", (unsigned long long)a, d, o, (unsigned long long)card);
                    extern void memwatch_watch(uint64_t addr);
                    for (int w = 0; w < 0x310; w += 0x10) memwatch_watch(card + w);
                    return;
                }
            for (uint64_t o = 0; d < 2 && o < 0x100; o += 8) {
                uint64_t v = U64(lvl[d][k] + o);
                if (guest_ptr(v) && n[d + 1] < 64) lvl[d + 1][n[d + 1]++] = v;
            }
        }
    LOG("[bcat-trace] marker not found from %llx\n", (unsigned long long)c0);
}
static int bcat_trace_job(Cpu *c, uint64_t addr) {
    uint64_t j = c->x[0], r = U64(j + 0x38);
    uint32_t st = U32(j + 0x14), i = U32(j + 0x58), err = r ? U32(r + 0x20) : 0;
    rt_call_original(c, addr);
    uint32_t st2 = U32(j + 0x14), err2 = r ? U32(r + 0x20) : 0;
    if (st != st2 || err != err2) LOG("[bcat-trace] job %llx: state %u -> %u, index %u, result %x -> %x (ret %llu)\n", (unsigned long long)j, st, st2, i, err, err2, (unsigned long long)c->x[0]);
#ifdef _WIN32
    if (st2 == 4 && st == 5 && g_trace_container && getenv("LARECOMP_WATCH_READ")) bcat_trace_find(g_trace_container);  // (normal loaded)
#endif
    return 1;
}
static int bcat_trace_apply(Cpu *c, uint64_t addr) {
    LOG("[bcat-trace] apply container %llx (x1 %llx x2 %llx)\n", (unsigned long long)c->x[3], (unsigned long long)c->x[1], (unsigned long long)c->x[2]);
    g_trace_container = c->x[3];
    rt_call_original(c, addr);
    LOG("[bcat-trace] apply -> %llx\n", (unsigned long long)c->x[0]);
    return 1;
}
__attribute__((constructor)) static void bcat_trace_init(void) {
    if (!getenv("LARECOMP_BCAT_TRACE")) return;
    extern void rt_register_fnhook_raw(uint64_t addr, int (*fn)(Cpu *c, uint64_t addr));
    rt_register_fnhook_raw(0x7000b2f5d0, bcat_trace_job);
    rt_register_fnhook_raw(0x7000112530, bcat_trace_apply);
}

SERVICE("bcat:u", bcat_u) {
    (void)s;
    switch (req->cmd) {
    case 0: ipc_return_object(resp, service_new("IBcatService", bcat_service, NULL)); break;                     // CreateBcatService
    case 1: case 2: ipc_return_object(resp, service_new("IDeliveryCacheStorageService", storage_service, NULL)); break;
    default: LOG("[bcat] bcat:u cmd %u (ok)\n", req->cmd);
    }
}
