#include "account.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static AccountSnapshot g_view;
static char g_browser[8192];
static AccountProfileSnapshot g_profile;
static AccountInboxSnapshot g_inbox;
static AccountClaimSnapshot g_claim;
void account_profile_snapshot(AccountProfileSnapshot *out) { pthread_mutex_lock(&g_lock); *out = g_profile; pthread_mutex_unlock(&g_lock); }
void account_inbox_snapshot(AccountInboxSnapshot *out) { pthread_mutex_lock(&g_lock); *out = g_inbox; pthread_mutex_unlock(&g_lock); }
void account_claim_snapshot(AccountClaimSnapshot *out) { pthread_mutex_lock(&g_lock); *out=g_claim; pthread_mutex_unlock(&g_lock); }

void account_snapshot(AccountSnapshot *out) {
    pthread_mutex_lock(&g_lock); *out = g_view; pthread_mutex_unlock(&g_lock);
}
bool account_take_browser_url(char *out, size_t size) {
    bool ok = false;
    pthread_mutex_lock(&g_lock);
    if (g_browser[0] && strlen(g_browser) < size) {
        memcpy(out, g_browser, strlen(g_browser) + 1); g_browser[0] = 0; ok = true;
    }
    pthread_mutex_unlock(&g_lock);
    return ok;
}
static void view(AccountState state, const char *detail) {
    pthread_mutex_lock(&g_lock);
    g_view.state = state;
    snprintf(g_view.detail, sizeof g_view.detail, "%s", detail);
    pthread_mutex_unlock(&g_lock);
}

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <wincred.h>
#include <bcrypt.h>
#include <wincrypt.h>
#define JSMN_STATIC
#define JSMN_STRICT
#define JSMN_PARENT_LINKS
#include "../third_party/jsmn/jsmn.h"

typedef struct { WCHAR host[256], path[4096]; INTERNET_PORT port; bool secure, loopback; } Url;
typedef struct { char access[512], refresh[512]; ULONGLONG expires; } Session;
static char g_base[512];
static WCHAR g_target[256];
static Session g_session;
static pthread_t g_thread;
static bool g_started;
static atomic_bool g_remember;
static atomic_bool g_cancel, g_stop;
static int g_command;
enum { CMD_NONE, CMD_RESTORE, CMD_LOGIN, CMD_LOGOUT, CMD_CANCEL, CMD_PROFILE_BIND, CMD_INBOX,
       CMD_PREVIEW, CMD_CLAIM, CMD_RESUME_CLAIM, CMD_FORGET_CLAIM };
static char g_identity[64];
static char g_local_input[1024], g_local_edition[16], g_bind_label[256];
static WCHAR g_local_directory[4096];
static unsigned g_context_generation, g_inbox_generation;
static bool g_profile_reload;
static void account_data_clear_locked(void);
static void profile_restore(void);
static void profile_bind(void);
static void inbox_fetch(void);
static void claim_reset_locked(void);
static bool claim_busy_locked(void);
static void claim_preview(void);
static void claim_submit(bool resume);
static void claim_restore(void);
static void claim_forget(void);
static bool g_claim_reload;

static bool utf16(const char *s, WCHAR *out, size_t n) {
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, out, (int)n) > 0;
}
static bool parse_url(const char *text, Url *out, bool query_allowed) {
    WCHAR wide[4096], user[128], password[128], extra[2048];
    if (!utf16(text, wide, sizeof wide/sizeof *wide)) return false;
    URL_COMPONENTS u = {.dwStructSize = sizeof u, .lpszHostName = out->host,
        .dwHostNameLength = sizeof out->host/sizeof *out->host - 1, .lpszUrlPath = out->path,
        .dwUrlPathLength = sizeof out->path/sizeof *out->path - 1, .lpszUserName = user,
        .dwUserNameLength = 127, .lpszPassword = password, .dwPasswordLength = 127,
        .lpszExtraInfo = extra, .dwExtraInfoLength = 2047};
    if (!WinHttpCrackUrl(wide, 0, 0, &u) || u.dwUserNameLength || u.dwPasswordLength) return false;
    out->host[u.dwHostNameLength] = 0; out->path[u.dwUrlPathLength] = 0;
    if (!out->path[0]) wcscpy(out->path, L"/");
    if (u.dwExtraInfoLength) {
        extra[u.dwExtraInfoLength]=0;
        if (!query_allowed || extra[0]!='?' || wcschr(extra,L'#') || wcslen(out->path)+u.dwExtraInfoLength>=4096) return false;
        wcscat(out->path,extra);
    }
    out->port = u.nPort; out->secure = u.nScheme == INTERNET_SCHEME_HTTPS;
    out->loopback = !wcscmp(out->host, L"127.0.0.1") || !_wcsicmp(out->host, L"localhost") || !wcscmp(out->host, L"::1");
    return out->secure || (u.nScheme == INTERNET_SCHEME_HTTP && out->loopback);
}
static bool url(const char *text, Url *out) { return parse_url(text,out,false); }
static bool hash(const void *data, size_t n, unsigned char out[32]) {
    BCRYPT_ALG_HANDLE alg = NULL;
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0;
    if (ok) ok = BCryptHash(alg, NULL, 0, (PUCHAR)data, (ULONG)n, out, 32) == 0;
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}
static bool random_hex(char out[65]) {
    unsigned char data[32];
    if (BCryptGenRandom(NULL, data, sizeof data, BCRYPT_USE_SYSTEM_PREFERRED_RNG)) return false;
    for (int i = 0; i < 32; i++) sprintf(out + i*2, "%02x", data[i]);
    SecureZeroMemory(data, sizeof data); return true;
}
static bool equal(const char *a, const char *b) {
    size_t n = strlen(a); if (n != strlen(b)) return false;
    unsigned char diff = 0; for (size_t i = 0; i < n; i++) diff |= a[i] ^ b[i];
    return diff == 0;
}
static bool unreserved(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("-._~", c) != NULL;
}
static bool safe_token(const char *s) {
    if (!*s) return false;
    for (; *s; s++) if (!unreserved((unsigned char)*s)) return false;
    return true;
}
static bool encode(const char *s, char *out, size_t cap) {
    size_t n = 0;
    for (; *s; s++) {
        unsigned char c = *s;
        if (n + (unreserved(c) ? 1 : 3) >= cap) return false;
        if (unreserved(c)) out[n++] = c;
        else { sprintf(out + n, "%%%02X", c); n += 3; }
    }
    out[n] = 0; return true;
}
static int hex(char c) {
    if (c >= '0' && c <= '9') return c-'0';
    if (c >= 'a' && c <= 'f') return c-'a'+10;
    if (c >= 'A' && c <= 'F') return c-'A'+10;
    return -1;
}
static bool decode_query(const char *s, size_t n, char *out, size_t cap) {
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = s[i];
        if (c == '%') {
            if (i+2 >= n || hex(s[i+1]) < 0 || hex(s[i+2]) < 0) return false;
            c = hex(s[i+1])*16 + hex(s[i+2]); i += 2;
        } else if (c == '+') c = ' ';
        if (c < 32 || c == 127 || k+1 >= cap) return false;
        out[k++] = c;
    }
    out[k] = 0; return true;
}
static bool json_get(const char *json, const char *key, char *out, size_t cap, bool number) {
    jsmn_parser parser; jsmntok_t t[256]; jsmn_init(&parser);
    int count = jsmn_parse(&parser, json, strlen(json), t, 256), found = -1;
    if (count < 1 || t[0].type != JSMN_OBJECT) return false;
    for (int i = 1; i+1 < count; i++) {
        if (t[i].parent != 0 || t[i].type != JSMN_STRING || t[i].end-t[i].start != (int)strlen(key) ||
            memcmp(json+t[i].start, key, strlen(key))) continue;
        if (found >= 0) return false;
        found = i+1;
    }
    if (found < 0 || t[found].type != (number ? JSMN_PRIMITIVE : JSMN_STRING)) return false;
    size_t k = 0;
    for (int i = t[found].start; i < t[found].end; i++) {
        unsigned char c = json[i];
        if (c == '\\') {
            if (++i >= t[found].end) return false;
            c = json[i];
            if (c == 'u') {
                unsigned value = 0;
                for (int j = 0; j < 4; j++) { if (++i >= t[found].end || hex(json[i]) < 0) return false; value = value*16 + hex(json[i]); }
                if (value >= 0xd800 && value <= 0xdbff) {
                    if (i+6 >= t[found].end || json[++i] != '\\' || json[++i] != 'u') return false;
                    unsigned low = 0;
                    for (int j = 0; j < 4; j++) { if (++i >= t[found].end || hex(json[i]) < 0) return false; low = low*16 + hex(json[i]); }
                    if (low < 0xdc00 || low > 0xdfff) return false;
                    value = 0x10000 + (value-0xd800)*1024 + low-0xdc00;
                } else if (value >= 0xdc00 && value <= 0xdfff) return false;
                if (value < 32 || value == 127 || k+4 >= cap) return false;
                if (value < 128) out[k++] = value;
                else if (value < 2048) { out[k++] = 0xc0|(value>>6); out[k++] = 0x80|(value&63); }
                else if (value < 65536) { out[k++] = 0xe0|(value>>12); out[k++] = 0x80|((value>>6)&63); out[k++] = 0x80|(value&63); }
                else { out[k++] = 0xf0|(value>>18); out[k++] = 0x80|((value>>12)&63); out[k++] = 0x80|((value>>6)&63); out[k++] = 0x80|(value&63); }
                continue;
            }
            if (c != '\\' && c != '"' && c != '/') return false;
        }
        if (c < 32 || c == 127 || k+1 >= cap) return false;
        out[k++] = c;
    }
    out[k] = 0;
    if (number && (!k || strspn(out, "0123456789") != k)) return false;
    return true;
}
static bool http_json_extra(const char *method, const char *path, const char *body, const char *access, const char *extra,
                      char *out, size_t cap, int *status) {
    char target[4096], headers[1024]; WCHAR verb[16], wh[1024]; Url u;
    *status = 0; out[0] = 0;
    if (snprintf(target, sizeof target, "%s%s", g_base, path) >= (int)sizeof target || !parse_url(target, &u, true) || !utf16(method, verb, 16)) return false;
    if(snprintf(headers, sizeof headers, "Content-Type: application/json\r\nAccept: application/json\r\n%s%s%s%s",
             access && *access ? "Authorization: Bearer " : "", access && *access ? access : "", access && *access ? "\r\n" : "",extra ? extra : "") >= (int)sizeof headers) return false;
    if (!utf16(headers, wh, 1024)) return false;
    HINTERNET session = WinHttpOpen(L"LetsGoRecomp/0.7 Account", u.loopback ? WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connection = NULL, request = NULL; bool ok = false;
    if (!session) goto done;
    WinHttpSetTimeouts(session, 3000, 5000, 5000, 5000);
    connection = WinHttpConnect(session, u.host, u.port, 0);
    if (!connection) goto done;
    request = WinHttpOpenRequest(connection, verb, u.path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 u.secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request) goto done;
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER, disable = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
    if (!WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof policy) ||
        !WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof disable)) goto done;
    if (!WinHttpSendRequest(request, wh, (DWORD)-1, (void *)body, body ? (DWORD)strlen(body) : 0,
                            body ? (DWORD)strlen(body) : 0, 0) || !WinHttpReceiveResponse(request, NULL)) goto done;
    DWORD code, size = sizeof code;
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &code, &size, NULL)) goto done;
    *status = (int)code;
    size_t n = 0; ULONGLONG deadline = GetTickCount64() + 15000;
    while (n+1 < cap && GetTickCount64() < deadline) {
        DWORD got;
        if (!WinHttpReadData(request, out+n, (DWORD)(cap-1-n), &got)) goto done;
        if (!got) { out[n] = 0; ok = true; break; }
        n += got;
    }
done:
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);
    SecureZeroMemory(headers, sizeof headers); SecureZeroMemory(wh, sizeof wh);
    return ok;
}
static bool http_json(const char *method,const char *path,const char *body,const char *access,char *out,size_t cap,int *status) {
    return http_json_extra(method,path,body,access,NULL,out,cap,status);
}
static bool stored_read(char out[512]) {
    PCREDENTIALW cred = NULL; bool ok = false;
    if (CredReadW(g_target, CRED_TYPE_GENERIC, 0, &cred)) {
        if (cred->CredentialBlobSize > 0 && cred->CredentialBlobSize < 512) {
            memcpy(out, cred->CredentialBlob, cred->CredentialBlobSize); out[cred->CredentialBlobSize] = 0;
            ok = safe_token(out);
        }
        SecureZeroMemory(cred->CredentialBlob, cred->CredentialBlobSize); CredFree(cred);
    }
    return ok;
}
static void stored_delete(void) { CredDeleteW(g_target, CRED_TYPE_GENERIC, 0); }
static bool stored_write(const char *token) {
    CREDENTIALW cred = {.Type = CRED_TYPE_GENERIC, .TargetName = g_target,
        .CredentialBlobSize = (DWORD)strlen(token), .CredentialBlob = (LPBYTE)token,
        .Persist = CRED_PERSIST_LOCAL_MACHINE, .UserName = L"Let's Go Recomp account"};
    return CredWriteW(&cred, 0) != FALSE;
}
static void revoke(Session *session) {
    if (!session->access[0]) return;
    char result[4096]; int status;
    http_json("POST", "/v1/auth/logout", NULL, session->access, result, sizeof result, &status);
    SecureZeroMemory(result, sizeof result);
}
static void clear_session(void) {
    SecureZeroMemory(&g_session, sizeof g_session);
    pthread_mutex_lock(&g_lock); g_view.name[0] = 0; g_view.remembered = false; g_identity[0] = 0;
    account_data_clear_locked(); pthread_mutex_unlock(&g_lock);
}
static bool accept_session(const char *result, bool resuming) {
    Session fresh = {0}; char seconds[16], me[4096], name[256], identity[64]; int status;
    if (!json_get(result, "access_token", fresh.access, sizeof fresh.access, false) ||
        !json_get(result, "refresh_token", fresh.refresh, sizeof fresh.refresh, false) ||
        !json_get(result, "expires_in", seconds, sizeof seconds, true) ||
        !safe_token(fresh.access) || !safe_token(fresh.refresh)) return false;
    long expiry = atol(seconds);
    if (expiry < 1 || expiry > 3600) { SecureZeroMemory(&fresh, sizeof fresh); return false; }
    fresh.expires = GetTickCount64() + (ULONGLONG)expiry*1000;
    if (resuming && atomic_load(&g_stop) && g_remember && stored_write(fresh.refresh)) {
        // Closing during refresh must not leave the already-consumed old credential in the vault.
        g_session = fresh; SecureZeroMemory(&fresh, sizeof fresh); return false;
    }
    if (atomic_load(&g_cancel) || atomic_load(&g_stop)) { revoke(&fresh); SecureZeroMemory(&fresh, sizeof fresh); return false; }
    // Persist the rotated refresh credential before any further network operation.
    bool remembered = g_remember && stored_write(fresh.refresh);
    if (!g_remember || !remembered) stored_delete();
    g_session = fresh; SecureZeroMemory(&fresh, sizeof fresh);
    if (!http_json("GET", "/v1/me", NULL, g_session.access, me, sizeof me, &status) || status != 200 ||
        !json_get(me, "display_name", name, sizeof name, false) || !json_get(me, "id", identity, sizeof identity, false)) {
        view(ACCOUNT_ERROR, "Signed in, but account details could not be loaded. Retry to reconnect.");
        SecureZeroMemory(me, sizeof me); return false;
    }
    if (atomic_load(&g_cancel) || atomic_load(&g_stop)) {
        if (!(resuming && atomic_load(&g_stop) && remembered)) { revoke(&g_session); stored_delete(); clear_session(); }
        SecureZeroMemory(me, sizeof me); return false;
    }
    pthread_mutex_lock(&g_lock);
    if (strcmp(g_identity, identity)) { account_data_clear_locked(); g_profile_reload = true; }
    snprintf(g_identity, sizeof g_identity, "%s", identity);
    snprintf(g_view.name, sizeof g_view.name, "%s", name); g_view.remembered = remembered;
    pthread_mutex_unlock(&g_lock);
    view(ACCOUNT_SIGNED_IN, g_remember && !remembered ? "Signed in for this session. Windows could not save the sign-in." : "Your account is connected.");
    SecureZeroMemory(me, sizeof me); return true;
}
static void restore(void) {
    char token[512] = {0}, body[640], result[8192]; int status;
    bool memory = g_session.refresh[0] != 0;
    if (memory) snprintf(token, sizeof token, "%s", g_session.refresh);
    else if (!stored_read(token)) { view(ACCOUNT_OFFLINE, "Sign in to connect your account."); return; }
    g_remember = !memory || g_remember;
    view(ACCOUNT_RESTORING, "Reconnecting your account...");
    snprintf(body, sizeof body, "{\"refresh_token\":\"%s\"}", token);
    bool ok = http_json("POST", "/v1/auth/refresh", body, NULL, result, sizeof result, &status);
    if (ok && status == 200) {
        if (!accept_session(result, true) && !atomic_load(&g_cancel) && !atomic_load(&g_stop) && !g_session.access[0])
            view(ACCOUNT_ERROR, "The account service returned an invalid response. Sign in again.");
    } else if (ok && (status == 401 || status == 403)) {
        stored_delete(); clear_session(); view(ACCOUNT_OFFLINE, "Your session expired. Sign in again.");
    } else {
        view(ACCOUNT_ERROR, "Could not reach the account service. You can retry or keep playing offline.");
    }
    SecureZeroMemory(token, sizeof token); SecureZeroMemory(body, sizeof body); SecureZeroMemory(result, sizeof result);
}

#include "account_data.inc"
#include "account_claim.inc"

static void reply(SOCKET client, int status, const char *message) {
    char response[2048];
    int n = snprintf(response, sizeof response,
        "HTTP/1.1 %d %s\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\n"
        "Content-Security-Policy: default-src 'none'; frame-ancestors 'none'\r\nReferrer-Policy: no-referrer\r\n"
        "Connection: close\r\nContent-Length: %zu\r\n\r\n%s", status, status == 200 ? "OK" : "Bad Request", strlen(message), message);
    send(client, response, n, 0);
}
static bool query_field(const char *query, const char *name, char *out, size_t cap) {
    bool seen = false; out[0] = 0;
    for (const char *p = query; *p;) {
        const char *end = strchr(p, '&'); if (!end) end = p+strlen(p);
        const char *eq = memchr(p, '=', end-p);
        if (eq && (size_t)(eq-p) == strlen(name) && !memcmp(p, name, eq-p)) {
            if (seen || !decode_query(eq+1, end-eq-1, out, cap)) return false;
            seen = true;
        }
        p = *end ? end+1 : end;
    }
    return true;
}
static bool receive_callback(SOCKET listener, const char *state, const char *issuer, char code[4097]) {
    ULONGLONG timeout = 180000;
    const char *qa = getenv("LARECOMP_ACCOUNT_QA_TIMEOUT_MS");
    if (qa && getenv("LARECOMP_ACCOUNT_QA_DIR")) { long t = atol(qa); if (t >= 100 && t <= 180000) timeout = t; }
    ULONGLONG end = GetTickCount64() + timeout;
    while (!atomic_load(&g_cancel) && !atomic_load(&g_stop) && GetTickCount64() < end) {
        fd_set reads; FD_ZERO(&reads); FD_SET(listener, &reads); struct timeval wait = {0, 100000};
        if (select(0, &reads, NULL, NULL, &wait) <= 0) continue;
        SOCKET client = accept(listener, NULL, NULL); if (client == INVALID_SOCKET) continue;
        DWORD limit = 200; setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char *)&limit, sizeof limit);
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, (const char *)&limit, sizeof limit);
        char request[8192] = {0}; size_t used = 0; ULONGLONG until = GetTickCount64()+2000;
        while (used+1 < sizeof request && GetTickCount64() < until && !atomic_load(&g_cancel) && !atomic_load(&g_stop)) {
            int n = recv(client, request+used, sizeof request-1-used, 0);
            if (n > 0) { used += n; request[used] = 0; if (strstr(request, "\r\n\r\n")) break; }
            else if (n == 0 || WSAGetLastError() != WSAETIMEDOUT) break;
        }
        char path[8192], got_state[129], got_issuer[1024], error[129];
        char version[16];
        bool valid = strstr(request, "\r\n\r\n") && sscanf(request, "GET %8191s %15s", path, version) == 2 &&
            (!strcmp(version, "HTTP/1.1") || !strcmp(version, "HTTP/1.0")) && !strncmp(path, "/callback?", 10);
        if (valid) {
            valid = query_field(path+10, "state", got_state, sizeof got_state) && equal(got_state, state) &&
                query_field(path+10, "code", code, 4097) && query_field(path+10, "error", error, sizeof error) &&
                query_field(path+10, "iss", got_issuer, sizeof got_issuer) && (!got_issuer[0] || equal(got_issuer, issuer)) &&
                ((code[0] && !error[0] && safe_token(code)) || (!code[0] && error[0]));
        }
        reply(client, valid ? 200 : 400, valid ? "<!doctype html><title>Let's Go Recomp</title><p>Sign-in received. You can return to Let's Go Recomp.</p>" :
              "<!doctype html><title>Let's Go Recomp</title><p>This callback was not accepted. Continue signing in from the launcher.</p>");
        closesocket(client); SecureZeroMemory(request, sizeof request); SecureZeroMemory(path, sizeof path);
        if (valid) {
            if (error[0]) { view(ACCOUNT_OFFLINE, "Sign-in was cancelled in the browser."); return false; }
            return true;
        }
    }
    view(ACCOUNT_OFFLINE, atomic_load(&g_cancel) || atomic_load(&g_stop) ? "Sign-in cancelled." : "Sign-in timed out. You can try again.");
    return false;
}
static void login(void) {
    char result[16384], issuer[1024], endpoint[2048], client_id[129], redirect[256], state[65], nonce[65], verifier[65];
    char eclient[512], eredirect[1024], browser[8192], body[8192], code[4097], challenge[44];
    unsigned char hashed[32]; SOCKET listener = INVALID_SOCKET; int status;
    view(ACCOUNT_COMPLETING, "Connecting to the sign-in service...");
    if (!http_json("GET", "/v1/auth/native/config", NULL, NULL, result, sizeof result, &status) || status != 200) {
        view(ACCOUNT_ERROR, "Could not reach the sign-in service. Check the connection and retry."); goto done;
    }
    Url provider, callback;
    if (!json_get(result, "issuer", issuer, sizeof issuer, false) || !json_get(result, "authorization_endpoint", endpoint, sizeof endpoint, false) ||
        !json_get(result, "client_id", client_id, sizeof client_id, false) || !json_get(result, "redirect_uri", redirect, sizeof redirect, false) ||
        !url(endpoint, &provider) || !url(redirect, &callback) || callback.secure || wcscmp(callback.host, L"127.0.0.1") ||
        wcscmp(callback.path, L"/callback") || !callback.port) {
        view(ACCOUNT_ERROR, "The account service has an invalid sign-in configuration."); goto done;
    }
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) { view(ACCOUNT_ERROR, "Could not create the browser callback listener."); goto done; }
    BOOL exclusive = TRUE;
    setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&exclusive, sizeof exclusive);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(callback.port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (bind(listener, (struct sockaddr *)&address, sizeof address) || listen(listener, 4)) {
        view(ACCOUNT_ERROR, "The sign-in callback port is busy. Finish or cancel the other sign-in, then retry."); goto done;
    }
    if (!random_hex(state) || !random_hex(nonce) || !random_hex(verifier) || !hash(verifier, strlen(verifier), hashed)) {
        view(ACCOUNT_ERROR, "Windows could not create a secure sign-in request."); goto done;
    }
    const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    unsigned bits = 0, value = 0; size_t cn = 0;
    for (int i = 0; i < 32; i++) {
        value = (value<<8)|hashed[i]; bits += 8;
        while (bits >= 6) { bits -= 6; challenge[cn++] = alphabet[(value>>bits)&63]; }
    }
    if (bits) challenge[cn++] = alphabet[(value<<(6-bits))&63]; challenge[cn] = 0;
    if (!encode(client_id, eclient, sizeof eclient) || !encode(redirect, eredirect, sizeof eredirect)) goto done;
    if (snprintf(browser, sizeof browser, "%s?client_id=%s&redirect_uri=%s&response_type=code&scope=openid%%20profile&"
                 "code_challenge=%s&code_challenge_method=S256&state=%s&nonce=%s", endpoint, eclient, eredirect, challenge, state, nonce) >= (int)sizeof browser) goto done;
    if (atomic_load(&g_cancel) || atomic_load(&g_stop)) goto done;
    pthread_mutex_lock(&g_lock); snprintf(g_browser, sizeof g_browser, "%s", browser); pthread_mutex_unlock(&g_lock);
    view(ACCOUNT_WAITING, "Complete sign-in in your browser. You can keep playing offline.");
    if (!receive_callback(listener, state, issuer, code)) goto done;
    closesocket(listener); listener = INVALID_SOCKET;
    if (atomic_load(&g_cancel) || atomic_load(&g_stop)) goto done;
    view(ACCOUNT_COMPLETING, "Verifying your sign-in...");
    snprintf(body, sizeof body, "{\"code\":\"%s\",\"code_verifier\":\"%s\",\"nonce\":\"%s\"}", code, verifier, nonce);
    if (!http_json("POST", "/v1/auth/native/complete", body, NULL, result, sizeof result, &status) || status != 200) {
        view(ACCOUNT_ERROR, "Sign-in could not be completed. Please try again."); goto done;
    }
    if (!accept_session(result, false) && !atomic_load(&g_cancel) && !atomic_load(&g_stop) && !g_session.access[0])
        view(ACCOUNT_ERROR, "The account service returned an invalid sign-in response.");
done:
    if (listener != INVALID_SOCKET) closesocket(listener);
    pthread_mutex_lock(&g_lock); g_browser[0] = 0; pthread_mutex_unlock(&g_lock);
    SecureZeroMemory(result, sizeof result); SecureZeroMemory(body, sizeof body); SecureZeroMemory(code, sizeof code);
    SecureZeroMemory(verifier, sizeof verifier); SecureZeroMemory(nonce, sizeof nonce); SecureZeroMemory(state, sizeof state);
}
static void *worker(void *unused) {
    (void)unused;
    while (!atomic_load(&g_stop)) {
        pthread_mutex_lock(&g_lock); int command = g_command; g_command = CMD_NONE; pthread_mutex_unlock(&g_lock);
        if (command == CMD_LOGIN) login();
        else if (command == CMD_RESTORE) restore();
        else if (command == CMD_PROFILE_BIND) profile_bind();
        else if (command == CMD_INBOX) inbox_fetch();
        else if (command == CMD_PREVIEW) claim_preview();
        else if (command == CMD_CLAIM || command == CMD_RESUME_CLAIM) claim_submit(command==CMD_RESUME_CLAIM);
        else if (command == CMD_FORGET_CLAIM) claim_forget();
        else if (command == CMD_CANCEL) {
            if (g_session.access[0]) { revoke(&g_session); stored_delete(); clear_session(); }
            view(ACCOUNT_OFFLINE, "Sign-in cancelled.");
        }
        else if (command == CMD_LOGOUT) {
            view(ACCOUNT_SIGNING_OUT, "Signing out...");
            // If access expired, refresh once so the server can revoke the session too.
            if (g_session.refresh[0] && GetTickCount64() >= g_session.expires) {
                atomic_store(&g_cancel, false); restore();
            }
            revoke(&g_session); stored_delete(); clear_session();
            view(ACCOUNT_OFFLINE, "Signed out on this device.");
        }
        if (!command) {
            pthread_mutex_lock(&g_lock);
            bool load_profile = g_profile_reload && g_view.state == ACCOUNT_SIGNED_IN;
            if (load_profile) g_profile_reload = false;
            pthread_mutex_unlock(&g_lock);
            if (load_profile) profile_restore();
            pthread_mutex_lock(&g_lock);
            bool load_claim=g_claim_reload && g_view.state==ACCOUNT_SIGNED_IN && g_profile.bound;
            if(load_claim) g_claim_reload=false;
            pthread_mutex_unlock(&g_lock);
            if(load_claim) claim_restore();
        }
        if (!command && g_session.access[0] && GetTickCount64()+60000 >= g_session.expires) {
            // Back off failed automatic refreshes rather than retrying every frame.
            static ULONGLONG retry_at;
            if (GetTickCount64() >= retry_at) { restore(); retry_at = GetTickCount64()+60000; }
        }
        Sleep(50);
    }
    if (!g_remember) revoke(&g_session);
    clear_session(); return NULL;
}
void account_start(void) {
    if (g_started) return;
    memset(&g_view, 0, sizeof g_view); g_browser[0] = 0;
    pthread_mutex_lock(&g_lock); account_data_clear_locked(); g_profile_reload = true; pthread_mutex_unlock(&g_lock);
    const char *base = getenv("LARECOMP_ACCOUNT_URL"); if (!base || !*base) base = "http://127.0.0.1:7385";
    if (strlen(base) >= sizeof g_base) { view(ACCOUNT_ERROR, "The account service address is too long."); return; }
    snprintf(g_base, sizeof g_base, "%s", base);
    size_t n = strlen(g_base); while (n && g_base[n-1] == '/') g_base[--n] = 0;
    Url parsed;
    if (!url(g_base, &parsed)) { view(ACCOUNT_ERROR, "Account services require HTTPS, or HTTP on localhost for development."); return; }
    snprintf(g_view.service, sizeof g_view.service, "%s", g_base);
    char namespace[1024]; unsigned char key[32];
    const char *scope = getenv("LARECOMP_ACCOUNT_STORAGE_SCOPE");
    if (snprintf(namespace, sizeof namespace, "%s|letsgo-native|%s", g_base, scope ? scope : "") >= (int)sizeof namespace || !hash(namespace, strlen(namespace), key)) {
        view(ACCOUNT_ERROR, "Could not initialize secure account storage."); return;
    }
    WCHAR suffix[65]; for (int i = 0; i < 32; i++) swprintf(suffix+i*2, 3, L"%02x", key[i]);
    swprintf(g_target, sizeof g_target/sizeof *g_target, L"LetsGoRecomp/Account/v1/%ls", suffix);
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data)) { view(ACCOUNT_ERROR, "Windows networking is unavailable."); return; }
    atomic_store(&g_stop, false); atomic_store(&g_cancel, false); g_command = CMD_RESTORE; g_remember = true;
    view(ACCOUNT_RESTORING, "Checking for a saved sign-in...");
    if (pthread_create(&g_thread, NULL, worker, NULL)) { WSACleanup(); view(ACCOUNT_ERROR, "Could not start the sign-in worker."); return; }
    g_started = true;
}
static void queue(int command) {
    if (!g_started) return;
    atomic_store(&g_cancel, command == CMD_CANCEL || command == CMD_LOGOUT);
    pthread_mutex_lock(&g_lock); g_command = command;
    if (command == CMD_LOGIN || command == CMD_LOGOUT || command == CMD_CANCEL) account_data_clear_locked();
    if (command == CMD_CANCEL || command == CMD_LOGOUT) g_browser[0] = 0;
    pthread_mutex_unlock(&g_lock);
}
void account_sign_in(bool remember) { g_remember = remember; queue(CMD_LOGIN); }
void account_retry(void) { queue(CMD_RESTORE); }
void account_cancel(void) { queue(CMD_CANCEL); }
void account_sign_out(void) { queue(CMD_LOGOUT); }
void account_shutdown(void) {
    if (!g_started) return;
    atomic_store(&g_stop, true); atomic_store(&g_cancel, true);
    pthread_join(g_thread, NULL); g_started = false; WSACleanup();
    pthread_mutex_lock(&g_lock); g_browser[0] = 0; pthread_mutex_unlock(&g_lock);
}
#else
void account_start(void) { view(ACCOUNT_UNSUPPORTED, "Account sign-in currently requires Windows."); }
void account_shutdown(void) {}
void account_sign_in(bool remember) { (void)remember; }
void account_cancel(void) {}
void account_sign_out(void) {}
void account_retry(void) {}
void account_select_local_profile(const char *save_dir, const char *edition) { (void)save_dir; (void)edition; }
void account_bind_profile(const char *label) { (void)label; }
void account_inbox_request(int kind, int page) { (void)kind; (void)page; }
void account_gift_preview(const char *id,const char *code) { (void)id;(void)code; }
void account_claim_confirm(void) {}
void account_claim_resume(void) {}
void account_claim_forget(void) {}
#endif
