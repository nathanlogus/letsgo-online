// Launcher account sessions. Passwords and provider tokens never enter game settings or saves.
#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    ACCOUNT_OFFLINE, ACCOUNT_RESTORING, ACCOUNT_WAITING, ACCOUNT_COMPLETING,
    ACCOUNT_SIGNED_IN, ACCOUNT_SIGNING_OUT, ACCOUNT_ERROR, ACCOUNT_UNSUPPORTED
} AccountState;
typedef struct {
    AccountState state;
    char name[256], detail[256], service[512];
    bool remembered;
} AccountSnapshot;

void account_start(void);
void account_shutdown(void);
void account_snapshot(AccountSnapshot *out);
void account_sign_in(bool remember);
void account_cancel(void);
void account_sign_out(void);
void account_retry(void);
// Browser opening belongs on the SDL/UI thread. URL contains state and the public PKCE challenge, never credentials.
bool account_take_browser_url(char *out, size_t size);

typedef struct {
    bool selected, bound, busy;
    unsigned revision;
    char edition[16], label[256], id[37], detail[256];
} AccountProfileSnapshot;
typedef struct {
    char id[37], title[512], status[16], profile_label[256];
    int item_count;
    bool requires_code;
} AccountInboxRow;
typedef enum { INBOX_SIGNED_OUT, INBOX_IDLE, INBOX_LOADING, INBOX_READY, INBOX_ERROR } AccountInboxState;
typedef struct {
    AccountInboxState state;
    int kind, page, count; // kinds: 0 available, 1 pending, 2 received
    bool has_more;
    char detail[256];
    AccountInboxRow rows[6];
} AccountInboxSnapshot;
// Local routing identity means edition + save directory. It is not ownership attestation.
void account_select_local_profile(const char *save_dir, const char *edition);
void account_profile_snapshot(AccountProfileSnapshot *out);
void account_bind_profile(const char *label);
void account_inbox_request(int kind, int page);
void account_inbox_snapshot(AccountInboxSnapshot *out);

typedef enum { CLAIM_IDLE, CLAIM_PREVIEWING, CLAIM_PREVIEW, CLAIM_SENDING, CLAIM_COMPLETE, CLAIM_ERROR } AccountClaimState;
typedef struct {
    AccountClaimState state;
    bool eligible, pending;
    int item_count;
    char title[512], profile_label[256], detail[256], id[37];
} AccountClaimSnapshot;
void account_claim_snapshot(AccountClaimSnapshot *out);
void account_gift_preview(const char *campaign_id, const char *code);
void account_claim_confirm(void);
void account_claim_resume(void);
void account_claim_forget(void); // Explicit local retry deletion only; never deletes server history.
