// Versioned per-profile retry journal, protected for the current Windows user.
// This allocates server entitlements only. It never writes native game rewards or acknowledgements.
#define NATIVE_ADAPTER "lgpe-wb7-v1"  // a native Mystery Gift card (runtime/account_delivery.inc)
typedef struct {
    uint32_t version;
    char account[37], profile[37], campaign[37], code[65], key[37], digest[65], adapter[32];
} PendingClaim;
static PendingClaim g_intent;
static unsigned g_intent_context;

static bool claim_busy_locked(void) { return g_claim.state==CLAIM_PREVIEWING || g_claim.state==CLAIM_SENDING; }
static void claim_reset_locked(void) { SecureZeroMemory(&g_intent,sizeof g_intent); memset(&g_claim,0,sizeof g_claim); g_intent_context=0; }
static bool code_normalize(const char *input,char out[65]) {
    const char *begin=input; while(*begin==' ') begin++;
    const char *end=begin+strlen(begin); while(end>begin && end[-1]==' ') end--;
    size_t n=0;
    for(const char *p=begin;p<end;p++) {
        unsigned char c=*p; if(c=='-') continue;
        if(c>='a' && c<='z') c-=32;
        if(!((c>='A' && c<='Z') || (c>='0' && c<='9')) || n>=64) return false;
        out[n++]=c;
    }
    out[n]=0; return n>=6;
}
static bool digest_valid(const char *s) { if(strlen(s)!=64) return false; for(int i=0;i<64;i++) if(hex(s[i])<0) return false; return true; }
static bool pending_valid(const PendingClaim *p,const ProfileContext *ctx,const char *profile) {
    if(p->version!=1 || !memchr(p->account,0,sizeof p->account) || !memchr(p->profile,0,sizeof p->profile) ||
       !memchr(p->campaign,0,sizeof p->campaign) || !memchr(p->code,0,sizeof p->code) || !memchr(p->key,0,sizeof p->key) ||
       !memchr(p->digest,0,sizeof p->digest) || !memchr(p->adapter,0,sizeof p->adapter)) return false;
    char normalized[65];
    return !strcmp(p->account,ctx->identity) && !strcmp(p->profile,profile) && uuid_valid(p->profile) && uuid_valid(p->key) &&
        digest_valid(p->digest) && (!strcmp(p->adapter,"lgpe-items-v1") || !strcmp(p->adapter,NATIVE_ADAPTER)) &&
        ((p->campaign[0] && !p->code[0] && uuid_valid(p->campaign)) ||
         (!p->campaign[0] && p->code[0] && code_normalize(p->code,normalized) && !strcmp(normalized,p->code)));
}
static bool pending_path(const ProfileContext *ctx,WCHAR out[4096]) {
    WCHAR metadata[4096]; return profile_path(ctx,metadata,false) && swprintf(out,4096,L"%ls.claim",metadata)>0;
}
static bool pending_entropy(const ProfileContext *ctx,const char *profile,unsigned char entropy[32]) {
    char scope[1024]; int n=snprintf(scope,sizeof scope,"%s|%s|%s|claim-v1",g_base,ctx->identity,profile);
    return n>0 && n<(int)sizeof scope && hash(scope,n,entropy);
}
static int pending_read(const ProfileContext *ctx,const WCHAR *path,const char *profile,PendingClaim *out) {
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if(file==INVALID_HANDLE_VALUE) return GetLastError()==ERROR_FILE_NOT_FOUND ? 0 : -1;
    unsigned char bytes[4096],entropy[32]; DWORD read=0; LARGE_INTEGER size;
    bool ok=GetFileSizeEx(file,&size) && size.QuadPart>0 && size.QuadPart<=sizeof bytes &&
        ReadFile(file,bytes,(DWORD)size.QuadPart,&read,NULL) && read==size.QuadPart;
    CloseHandle(file);
    if(!ok || !pending_entropy(ctx,profile,entropy)) return -1;
    DATA_BLOB input={read,bytes},extra={32,entropy},plain={0};
    ok=CryptUnprotectData(&input,NULL,&extra,NULL,NULL,CRYPTPROTECT_UI_FORBIDDEN,&plain)!=FALSE;
    if(ok && plain.cbData==sizeof *out) memcpy(out,plain.pbData,sizeof *out); else ok=false;
    if(plain.pbData) { SecureZeroMemory(plain.pbData,plain.cbData); LocalFree(plain.pbData); }
    SecureZeroMemory(bytes,sizeof bytes); SecureZeroMemory(entropy,sizeof entropy);
    if(!ok || !pending_valid(out,ctx,profile)) { SecureZeroMemory(out,sizeof *out); return -1; }
    return 1;
}
static bool pending_write(const ProfileContext *ctx,const WCHAR *path,const PendingClaim *p) {
    unsigned char entropy[32]; if(!pending_valid(p,ctx,p->profile) || !pending_entropy(ctx,p->profile,entropy)) return false;
    DATA_BLOB input={sizeof *p,(BYTE *)p},extra={32,entropy},encrypted={0};
    if(!CryptProtectData(&input,L"LetsGoRecomp gift retry",&extra,NULL,NULL,CRYPTPROTECT_UI_FORBIDDEN,&encrypted)) return false;
    WCHAR temp[4096],suffix[128]; char random[65]; bool ok=false;
    if(random_hex(random) && utf16(random,suffix,128) && swprintf(temp,4096,L"%ls.%ls.tmp",path,suffix)>0) {
        HANDLE file=CreateFileW(temp,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
        if(file!=INVALID_HANDLE_VALUE) {
            DWORD written; ok=WriteFile(file,encrypted.pbData,encrypted.cbData,&written,NULL) && written==encrypted.cbData && FlushFileBuffers(file);
            CloseHandle(file);
            if(ok) ok=MoveFileExW(temp,path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE;
            if(!ok) DeleteFileW(temp);
        }
    }
    SecureZeroMemory(encrypted.pbData,encrypted.cbData);LocalFree(encrypted.pbData);SecureZeroMemory(entropy,sizeof entropy);
    return ok;
}
static void claim_publish(const ProfileContext *ctx,AccountClaimState state,bool pending,const char *detail) {
    pthread_mutex_lock(&g_lock);
    if(ctx->generation==g_context_generation && g_view.state==ACCOUNT_SIGNED_IN && !strcmp(ctx->identity,g_identity)) {
        g_claim.state=state;g_claim.pending=pending;snprintf(g_claim.detail,sizeof g_claim.detail,"%s",detail);
    }
    pthread_mutex_unlock(&g_lock);
}
static void claim_restore(void) {
    ProfileContext ctx; WCHAR path[4096]; if(!profile_context(&ctx) || !pending_path(&ctx,path)) return;
    bool exists=GetFileAttributesW(path)!=INVALID_FILE_ATTRIBUTES;
    pthread_mutex_lock(&g_lock);
    if(ctx.generation==g_context_generation && !claim_busy_locked()) {
        g_claim.pending=exists;
        if(exists) { g_claim.eligible=false;snprintf(g_claim.detail,sizeof g_claim.detail,"A previous claim needs a retry. Resume it to recover the server result."); }
    }
    pthread_mutex_unlock(&g_lock);
}
void account_gift_preview(const char *campaign_id,const char *code) {
    PendingClaim intent={.version=1};
    if(code) { if(!code_normalize(code,intent.code)) {
        pthread_mutex_lock(&g_lock);if(!claim_busy_locked()) {g_claim.state=CLAIM_ERROR;g_claim.eligible=false;snprintf(g_claim.detail,sizeof g_claim.detail,"Use a gift code containing 6 to 64 letters or digits.");}pthread_mutex_unlock(&g_lock);return;
    } }
    else { if(!campaign_id || !uuid_valid(campaign_id)) return; snprintf(intent.campaign,sizeof intent.campaign,"%s",campaign_id); }
    pthread_mutex_lock(&g_lock);
    if(g_started && g_view.state==ACCOUNT_SIGNED_IN && g_profile.bound && !g_profile.busy && !claim_busy_locked() &&
       g_inbox.state!=INBOX_LOADING && g_command==CMD_NONE && !atomic_load(&g_cancel)) {
        snprintf(intent.account,sizeof intent.account,"%s",g_identity);snprintf(intent.profile,sizeof intent.profile,"%s",g_profile.id);
        snprintf(intent.adapter,sizeof intent.adapter,NATIVE_ADAPTER);  // (claim_preview falls back to lgpe-items-v1)
        g_intent=intent;g_intent_context=g_context_generation;memset(&g_claim,0,sizeof g_claim);g_claim.state=CLAIM_PREVIEWING;
        g_command=CMD_PREVIEW;
    }
    pthread_mutex_unlock(&g_lock);SecureZeroMemory(&intent,sizeof intent);
}
static bool claim_body(const PendingClaim *p,char body[512]) {
    return snprintf(body,512,"{\"%s\":\"%s\",\"profile_id\":\"%s\",\"adapter\":\"%s\"}",
        p->code[0] ? "code" : "campaign_id",p->code[0] ? p->code : p->campaign,p->profile,p->adapter)<512;
}
static bool json_boolean(const char *data,const char *name,bool *out) {
    jsmn_parser parser;jsmntok_t t[256];jsmn_init(&parser);int n=jsmn_parse(&parser,data,strlen(data),t,256);bool found=false;
    if(n<1 || t[0].type!=JSMN_OBJECT) return false;
    for(int i=1;i+1<n;i++) if(t[i].parent==0 && t[i].type==JSMN_STRING && t[i].end-t[i].start==(int)strlen(name) && !memcmp(data+t[i].start,name,strlen(name))) {
        if(found || t[i+1].type!=JSMN_PRIMITIVE) return false;found=true;int len=t[i+1].end-t[i+1].start;
        if(len==4 && !memcmp(data+t[i+1].start,"true",4)) *out=true;
        else if(len==5 && !memcmp(data+t[i+1].start,"false",5)) *out=false;else return false;
    }
    return found;
}
static void claim_preview(void) {
    ProfileContext ctx;PendingClaim intent;WCHAR path[4096];char body[512],result[12288],title[512],label[256],digest[65],profile[37],number[16],reason[80];bool eligible;int status;
    if(!profile_context(&ctx)) return;
    pthread_mutex_lock(&g_lock);intent=g_intent;unsigned generation=g_intent_context;pthread_mutex_unlock(&g_lock);
    if(generation!=ctx.generation || !claim_body(&intent,body)) goto done;
    if(!pending_path(&ctx,path)) { claim_publish(&ctx,CLAIM_ERROR,false,"Could not locate the claim retry journal.");goto done; }
    if(GetFileAttributesW(path)!=INVALID_FILE_ATTRIBUTES) { claim_publish(&ctx,CLAIM_ERROR,true,"Resume or clear the previous local retry before starting another claim.");goto done; }
    bool ok=authorized_json("POST","/v1/gift-preview",body,result,sizeof result,&status);
    // A campaign without native cards: the save-time route (lgpe-items-v1), previewed again with that adapter
    if(ok && status==200 && !strstr(result,"\""NATIVE_ADAPTER"\"") && strstr(result,"\"lgpe-items-v1\"")) {
        snprintf(intent.adapter,sizeof intent.adapter,"lgpe-items-v1");
        pthread_mutex_lock(&g_lock);if(g_intent_context==generation) snprintf(g_intent.adapter,sizeof g_intent.adapter,"lgpe-items-v1");pthread_mutex_unlock(&g_lock);
        ok=claim_body(&intent,body) && authorized_json("POST","/v1/gift-preview",body,result,sizeof result,&status);
    }
    if(!ok || status!=200 || !json_get(result,"title",title,sizeof title,false) ||
       !json_get(result,"profile_label",label,sizeof label,false) || !json_get(result,"profile_id",profile,sizeof profile,false) || strcmp(profile,intent.profile) ||
       !json_get(result,"reward_digest",digest,sizeof digest,false) || !digest_valid(digest) || !json_get(result,"item_count",number,sizeof number,true) ||
       !json_boolean(result,"eligible",&eligible) || !json_get(result,"reason",reason,sizeof reason,false)) {
        claim_publish(&ctx,CLAIM_ERROR,false,status==404 ? "Gift not found, or the code is not valid." : "Could not preview the gift. Please retry.");goto done;
    }
    long count=atol(number),species=json_long(result,"species",0),level=json_long(result,"level",0);
    if(count<0 || count>31968 || species<0 || species>809 || level<0 || level>100 || (!count && !species)) {claim_publish(&ctx,CLAIM_ERROR,false,"The gift preview was invalid.");goto done;}
    pthread_mutex_lock(&g_lock);
    if(ctx.generation==g_context_generation && g_intent_context==generation && g_view.state==ACCOUNT_SIGNED_IN) {
        snprintf(g_intent.digest,sizeof g_intent.digest,"%s",digest);g_claim.state=CLAIM_PREVIEW;g_claim.eligible=eligible;g_claim.item_count=(int)count;
        g_claim.species=(int)species;g_claim.level=(int)level;g_claim.native=!strcmp(intent.adapter,NATIVE_ADAPTER);
        snprintf(g_claim.title,sizeof g_claim.title,"%s",title);snprintf(g_claim.profile_label,sizeof g_claim.profile_label,"%s",label);
        const char *message=eligible ? (g_claim.native ? "Confirm to reserve this gift for the shown profile. Then, in the game: Mystery Gift > Get via internet." :
                                                         "Confirm to reserve this gift for the shown profile. It arrives in the game at your next save.") :
            !strcmp(reason,"already_claimed") ? "This account already claimed this gift. Check your gift history." :
            !strcmp(reason,"code_required") ? "Enter a gift code to preview this campaign." :
            !strcmp(reason,"incompatible_profile_or_adapter") ? "This gift does not support the selected profile." :
            !strcmp(reason,"out_of_stock") ? "This gift is out of stock." : "This gift is not currently claimable.";
        snprintf(g_claim.detail,sizeof g_claim.detail,"%s",message);
    }
    pthread_mutex_unlock(&g_lock);
done:
    SecureZeroMemory(&intent,sizeof intent);SecureZeroMemory(body,sizeof body);SecureZeroMemory(result,sizeof result);
}
static void claim_queue(int command) {
    pthread_mutex_lock(&g_lock);
    bool valid=command==CMD_CLAIM ? g_claim.state==CLAIM_PREVIEW && g_claim.eligible && g_intent_context==g_context_generation : g_claim.pending;
    if(g_started && g_view.state==ACCOUNT_SIGNED_IN && g_profile.bound && valid && !claim_busy_locked() && g_command==CMD_NONE && !atomic_load(&g_cancel)) {
        g_claim.state=CLAIM_SENDING;g_command=command;
    }
    pthread_mutex_unlock(&g_lock);
}
void account_claim_confirm(void) {claim_queue(CMD_CLAIM);}
void account_claim_resume(void) {claim_queue(CMD_RESUME_CLAIM);}
void account_claim_forget(void) {claim_queue(CMD_FORGET_CLAIM);}
static void claim_submit(bool resume) {
    ProfileContext ctx;PendingClaim p={0};WCHAR path[4096],lock_path[4096];char profile[37],body[512],headers[128],result[12288],id[37],digest[65],destination[37];int status=0;
    if(!profile_context(&ctx)) return;
    pthread_mutex_lock(&g_lock);snprintf(profile,sizeof profile,"%s",g_profile.id);if(!resume) p=g_intent;pthread_mutex_unlock(&g_lock);
    if(!pending_path(&ctx,path) || swprintf(lock_path,4096,L"%ls.lock",path)<0) {claim_publish(&ctx,CLAIM_ERROR,resume,"Could not locate the claim retry journal.");goto end;}
    HANDLE lock=CreateFileW(lock_path,GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if(lock==INVALID_HANDLE_VALUE) {claim_publish(&ctx,CLAIM_ERROR,resume,"Another launcher may be claiming this gift. Retry shortly.");goto end;}
    if(resume) {
        if(pending_read(&ctx,path,profile,&p)!=1) {claim_publish(&ctx,CLAIM_ERROR,true,"The local retry journal could not be read. Server claim history remains unchanged.");goto close;}
    } else {
        if(GetFileAttributesW(path)!=INVALID_FILE_ATTRIBUTES) {claim_publish(&ctx,CLAIM_ERROR,true,"A previous claim must be resumed first.");goto close;}
        if(!uuid_new(p.key) || !pending_valid(&p,&ctx,profile) || !pending_write(&ctx,path,&p)) {
            claim_publish(&ctx,CLAIM_ERROR,false,"The encrypted retry request could not be saved. No claim was sent.");goto close;
        }
    }
    if(!claim_body(&p,body)) goto close;
    snprintf(headers,sizeof headers,"Idempotency-Key: %s\r\n",p.key);
    bool ok=http_json_extra("POST","/v1/claims",body,g_session.access,headers,result,sizeof result,&status);
    if(ok && status==401 && !atomic_load(&g_cancel) && !atomic_load(&g_stop)) {
        restore();if(g_session.access[0]) ok=http_json_extra("POST","/v1/claims",body,g_session.access,headers,result,sizeof result,&status);
    }
    if(ok && status==201 && json_get(result,"id",id,sizeof id,false) && uuid_valid(id) &&
       json_get(result,"profile_id",destination,sizeof destination,false) && !strcmp(destination,p.profile) &&
       json_get(result,"reward_digest",digest,sizeof digest,false) && !strcmp(digest,p.digest)) {
        bool removed=DeleteFileW(path)!=FALSE;
        claim_publish(&ctx,CLAIM_COMPLETE,!removed,!strcmp(p.adapter,NATIVE_ADAPTER) ?
            "Gift claimed for this profile. In the game, open Mystery Gift > Get via internet to receive it." :
            "Gift claimed for this profile. It arrives in the game the next time you save.");
        pthread_mutex_lock(&g_lock);
        if(ctx.generation==g_context_generation && g_view.state==ACCOUNT_SIGNED_IN) {
            snprintf(g_claim.id,sizeof g_claim.id,"%s",id);g_claim.eligible=false;SecureZeroMemory(&g_intent,sizeof g_intent);
            g_delivery_reload=true;
            g_inbox_generation++;memset(&g_inbox,0,sizeof g_inbox);g_inbox.kind=-1;g_inbox.state=INBOX_IDLE;
        }
        pthread_mutex_unlock(&g_lock);
    } else claim_publish(&ctx,CLAIM_ERROR,true,status==409 ? "The server could not allocate this gift. Resume to retry or clear only this local retry request." :
        "The claim result could not be confirmed. Resume the saved request to recover it safely.");
close:
    CloseHandle(lock);
end:
    SecureZeroMemory(&p,sizeof p);SecureZeroMemory(body,sizeof body);SecureZeroMemory(result,sizeof result);
}
static void claim_forget(void) {
    ProfileContext ctx;WCHAR path[4096],lock_path[4096];if(!profile_context(&ctx) || !pending_path(&ctx,path) || swprintf(lock_path,4096,L"%ls.lock",path)<0) return;
    HANDLE lock=CreateFileW(lock_path,GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if(lock==INVALID_HANDLE_VALUE) {claim_publish(&ctx,CLAIM_ERROR,true,"The local retry request is busy. Please retry.");return;}
    bool removed=DeleteFileW(path)!=FALSE || GetLastError()==ERROR_FILE_NOT_FOUND;CloseHandle(lock);
    claim_publish(&ctx,removed ? CLAIM_IDLE : CLAIM_ERROR,!removed,removed ? "Local retry cleared. Your server claim history is unchanged." : "The local retry could not be cleared.");
    pthread_mutex_lock(&g_lock);if(ctx.generation==g_context_generation) SecureZeroMemory(&g_intent,sizeof g_intent);pthread_mutex_unlock(&g_lock);
}
