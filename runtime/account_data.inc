// Private account worker jobs. Included after the HTTP/session helpers; no game save parsing/writes.
typedef struct {
    unsigned generation;
    WCHAR directory[4096];
    char identity[64], edition[16];
} ProfileContext;

static void account_data_clear_locked(void) {
    g_context_generation++; g_inbox_generation++;
    claim_reset_locked(); g_claim_reload=true;
    memset(&g_profile, 0, sizeof g_profile); memset(&g_inbox, 0, sizeof g_inbox);
    g_profile.revision=g_context_generation;
    g_inbox.kind = -1; g_inbox.state = INBOX_SIGNED_OUT;
    g_profile.selected = g_local_directory[0] && g_local_edition[0];
    snprintf(g_profile.edition, sizeof g_profile.edition, "%s", g_local_edition);
    snprintf(g_profile.label, sizeof g_profile.label, "%s profile", !strcmp(g_local_edition,"pikachu") ? "Pikachu" : "Eevee");
    snprintf(g_profile.detail, sizeof g_profile.detail, "%s", "Link this local profile after signing in.");
}
static bool uuid_valid(const char *s) {
    if (strlen(s) != 36) return false;
    for (int i=0; i<36; i++) {
        bool dash = i==8 || i==13 || i==18 || i==23;
        if (dash ? s[i]!='-' : hex(s[i])<0) return false;
    }
    return true;
}
static bool uuid_new(char out[37]) {
    char raw[65]; if (!random_hex(raw)) return false;
    raw[12]='4'; raw[16]="89ab"[hex(raw[16])&3];
    snprintf(out,37,"%.8s-%.4s-%.4s-%.4s-%.12s",raw,raw+8,raw+12,raw+16,raw+20);
    return true;
}
static bool label_valid(const char *s) {
    WCHAR w[256]; if (!*s || !utf16(s,w,256)) return false;
    int count=0;
    for (int i=0; w[i]; i++) {
        if (w[i]<32 || w[i]==127 || ++count>64) return false;
        if (w[i]>=0xd800 && w[i]<=0xdbff && w[i+1]) i++;
    }
    return true;
}
static bool json_quote(const char *s, char *out, size_t cap) {
    size_t n=0; if (cap<3) return false; out[n++]='"';
    for (; *s; s++) {
        unsigned char c=*s; if (c<32 || c==127 || n+3>=cap) return false;
        if (c=='"' || c=='\\') out[n++]='\\';
        out[n++]=c;
    }
    out[n++]='"'; out[n]=0; return true;
}
void account_select_local_profile(const char *save_dir, const char *edition) {
    if (!save_dir || !edition || strlen(save_dir)>=sizeof g_local_input ||
        (strcmp(edition,"eevee") && strcmp(edition,"pikachu"))) return;
    pthread_mutex_lock(&g_lock);
    bool same=!strcmp(save_dir,g_local_input) && !strcmp(edition,g_local_edition);
    pthread_mutex_unlock(&g_lock);
    if (same) return;
    WCHAR input[4096], full[4096], canonical[4096];
    if (!utf16(save_dir,input,4096)) return;
    DWORD n=GetFullPathNameW(input,4096,full,NULL); if (!n || n>=4096) return;
    HANDLE folder=CreateFileW(full,0,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,NULL);
    if (folder!=INVALID_HANDLE_VALUE) {
        n=GetFinalPathNameByHandleW(folder,canonical,4096,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS); CloseHandle(folder);
        if (n && n<4096 && !wcsncmp(canonical,L"\\\\?\\",4)) wcscpy(full,canonical+4);
    }
    CharLowerBuffW(full,(DWORD)wcslen(full));
    pthread_mutex_lock(&g_lock);
    snprintf(g_local_input,sizeof g_local_input,"%s",save_dir); snprintf(g_local_edition,sizeof g_local_edition,"%s",edition);
    wcscpy(g_local_directory,full); account_data_clear_locked(); g_profile_reload=true;
    pthread_mutex_unlock(&g_lock);
}
static bool profile_context(ProfileContext *out) {
    pthread_mutex_lock(&g_lock);
    bool ok=g_view.state==ACCOUNT_SIGNED_IN && g_profile.selected && uuid_valid(g_identity);
    if (ok) {
        out->generation=g_context_generation; wcscpy(out->directory,g_local_directory);
        snprintf(out->identity,sizeof out->identity,"%s",g_identity); snprintf(out->edition,sizeof out->edition,"%s",g_local_edition);
    }
    pthread_mutex_unlock(&g_lock); return ok;
}
static void profile_publish(const ProfileContext *ctx, const char *id, const char *label, bool bound, const char *detail) {
    pthread_mutex_lock(&g_lock);
    if (ctx->generation==g_context_generation && !strcmp(ctx->identity,g_identity) && g_view.state==ACCOUNT_SIGNED_IN) {
        g_profile.busy=false; g_profile.bound=bound;
        if(bound) g_claim_reload=true;
        if (id) snprintf(g_profile.id,sizeof g_profile.id,"%s",id);
        if (label) snprintf(g_profile.label,sizeof g_profile.label,"%s",label);
        snprintf(g_profile.detail,sizeof g_profile.detail,"%s",detail);
    }
    pthread_mutex_unlock(&g_lock);
}
static bool profile_path(const ProfileContext *ctx, WCHAR out[4096], bool create) {
    WCHAR root[4096], resolved[4096], base[1024], id[128], edition[32]; unsigned char key[32], directory_hash[32];
    const char *override=getenv("LARECOMP_ACCOUNT_PROFILE_DIR");
    if (!utf16(override && *override ? override : "account-profiles",root,4096)) return false;
    DWORD n=GetFullPathNameW(root,4096,resolved,NULL); if (!n || n>=3500) return false;
    if (create && !CreateDirectoryW(resolved,NULL) && GetLastError()!=ERROR_ALREADY_EXISTS) return false;
    if (!utf16(g_base,base,1024) || !utf16(ctx->identity,id,128) || !utf16(ctx->edition,edition,32) ||
        !hash(ctx->directory,wcslen(ctx->directory)*sizeof(WCHAR),directory_hash)) return false;
    WCHAR fingerprint[65], input[1400];
    for (int i=0;i<32;i++) swprintf(fingerprint+i*2,3,L"%02x",directory_hash[i]);
    if (swprintf(input,1400,L"%ls|%ls|%ls|%ls",base,id,edition,fingerprint)<0 || !hash(input,wcslen(input)*sizeof(WCHAR),key)) return false;
    for (int i=0;i<32;i++) swprintf(fingerprint+i*2,3,L"%02x",key[i]);
    return swprintf(out,4096,L"%ls\\%ls.json",resolved,fingerprint)>0;
}
// 0 missing, 1 valid, -1 unreadable/inconsistent. Never silently replace invalid metadata.
static int profile_read(const ProfileContext *ctx, const WCHAR *path, char id[37], char label[256]) {
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if (file==INVALID_HANDLE_VALUE) return GetLastError()==ERROR_FILE_NOT_FOUND || GetLastError()==ERROR_PATH_NOT_FOUND ? 0 : -1;
    char data[2048], owner[64], edition[16], version[16]; DWORD read=0;
    LARGE_INTEGER size; bool ok=GetFileSizeEx(file,&size) && size.QuadPart>0 && size.QuadPart<sizeof data &&
        ReadFile(file,data,(DWORD)size.QuadPart,&read,NULL) && read==size.QuadPart;
    CloseHandle(file); if (!ok) return -1; data[read]=0;
    return json_get(data,"version",version,sizeof version,true) && !strcmp(version,"1") &&
        json_get(data,"profile_id",id,37,false) && uuid_valid(id) && json_get(data,"account_id",owner,sizeof owner,false) &&
        !strcmp(owner,ctx->identity) && json_get(data,"edition",edition,sizeof edition,false) && !strcmp(edition,ctx->edition) &&
        json_get(data,"label",label,256,false) && label_valid(label) ? 1 : -1;
}
static bool profile_write(const ProfileContext *ctx, const WCHAR *path, const char *id, const char *label) {
    char quoted[1024], body[2048], random[65]; WCHAR suffix[128], temp[4096];
    if (!json_quote(label,quoted,sizeof quoted) || !random_hex(random) || !utf16(random,suffix,128)) return false;
    int n=snprintf(body,sizeof body,"{\"version\":1,\"profile_id\":\"%s\",\"account_id\":\"%s\",\"edition\":\"%s\",\"label\":%s}\n",id,ctx->identity,ctx->edition,quoted);
    if (n<=0 || n>=(int)sizeof body || swprintf(temp,4096,L"%ls.%ls.tmp",path,suffix)<0) return false;
    HANDLE file=CreateFileW(temp,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL); if (file==INVALID_HANDLE_VALUE) return false;
    DWORD written; bool ok=WriteFile(file,body,n,&written,NULL) && written==(DWORD)n && FlushFileBuffers(file);
    CloseHandle(file);
    if (ok) ok=MoveFileExW(temp,path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE;
    if (!ok) DeleteFileW(temp);
    return ok;
}
static bool authorized_json(const char *method,const char *path,const char *body,char *out,size_t cap,int *status) {
    if (atomic_load(&g_cancel) || atomic_load(&g_stop) || !g_session.access[0]) { *status=401; return false; }
    bool ok=http_json(method,path,body,g_session.access,out,cap,status);
    if (ok && *status==401 && g_session.refresh[0] && !atomic_load(&g_cancel) && !atomic_load(&g_stop)) {
        restore();
        if (g_session.access[0]) ok=http_json(method,path,body,g_session.access,out,cap,status);
    }
    if (ok && (*status==401 || *status==403)) { stored_delete(); clear_session(); view(ACCOUNT_OFFLINE,"Your session expired. Sign in again."); }
    return ok;
}
static void profile_restore(void) {
    ProfileContext ctx; WCHAR path[4096]; char id[37], label[256], request[128], response[4096], remote[37], edition[16], remote_label[256]; int status;
    if (!profile_context(&ctx)) return;
    if (!profile_path(&ctx,path,false)) { profile_publish(&ctx,NULL,NULL,false,"Could not locate local profile metadata."); return; }
    int saved=profile_read(&ctx,path,id,label);
    if (!saved) { profile_publish(&ctx,NULL,NULL,false,"Link this save directory to your account. Game saves will stay unchanged."); return; }
    if (saved<0) { profile_publish(&ctx,NULL,NULL,false,"Local profile metadata is invalid or unreadable. No changes were made."); return; }
    snprintf(request,sizeof request,"/v1/profiles/%s",id);
    bool ok=authorized_json("GET",request,NULL,response,sizeof response,&status);
    if (ok && status==200 && json_get(response,"id",remote,sizeof remote,false) && !strcmp(remote,id) &&
        json_get(response,"edition",edition,sizeof edition,false) && !strcmp(edition,ctx.edition) &&
        json_get(response,"label",remote_label,sizeof remote_label,false) && !strcmp(label,remote_label)) {
        profile_publish(&ctx,id,label,true,"This local profile is linked to your account.");
    } else profile_publish(&ctx,id,label,false,ok && status==404 ? "This local profile is not yet linked. Link it to retry." : "Could not verify the profile binding. Retry when the service is available.");
}
void account_bind_profile(const char *label) {
    char clean[256]; snprintf(clean,sizeof clean,"%s",label ? label : "");
    size_t n=strlen(clean); while (n && clean[n-1]==' ') clean[--n]=0;
    size_t first=0; while(clean[first]==' ') first++; if(first) memmove(clean,clean+first,strlen(clean+first)+1);
    pthread_mutex_lock(&g_lock);
    if (g_started && g_view.state==ACCOUNT_SIGNED_IN && g_profile.selected && !g_profile.bound && !g_profile.busy &&
        g_inbox.state!=INBOX_LOADING && !claim_busy_locked() && g_command==CMD_NONE && !atomic_load(&g_cancel)) {
        if (!label_valid(clean)) snprintf(g_profile.detail,sizeof g_profile.detail,"Use a profile name between 1 and 64 characters.");
        else { snprintf(g_bind_label,sizeof g_bind_label,"%s",clean); g_profile.busy=true; g_command=CMD_PROFILE_BIND; }
    }
    pthread_mutex_unlock(&g_lock);
}
static void profile_bind(void) {
    ProfileContext ctx; WCHAR path[4096], lock_path[4096]; char id[37],label[256],quoted[1024],body[2048],response[4096],remote[37],edition[16],remote_label[256]; int status;
    if (!profile_context(&ctx)) return;
    pthread_mutex_lock(&g_lock); snprintf(label,sizeof label,"%s",g_bind_label); pthread_mutex_unlock(&g_lock);
    if (!profile_path(&ctx,path,true) || swprintf(lock_path,4096,L"%ls.lock",path)<0) { profile_publish(&ctx,NULL,NULL,false,"Could not create local profile metadata. No binding was sent."); return; }
    HANDLE lock=CreateFileW(lock_path,GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if(lock==INVALID_HANDLE_VALUE) { profile_publish(&ctx,NULL,NULL,false,"Another launcher may be linking this profile. Retry shortly."); return; }
    int saved=profile_read(&ctx,path,id,remote_label);
    if(saved<0) { profile_publish(&ctx,NULL,NULL,false,"Local profile metadata is invalid. No binding was sent."); goto done; }
    if(saved) snprintf(label,sizeof label,"%s",remote_label);
    else if(!uuid_new(id) || !profile_write(&ctx,path,id,label)) { profile_publish(&ctx,NULL,NULL,false,"Could not save the profile identity. No binding was sent."); goto done; }
    if(!json_quote(label,quoted,sizeof quoted)) goto done;
    snprintf(body,sizeof body,"{\"id\":\"%s\",\"edition\":\"%s\",\"label\":%s}",id,ctx.edition,quoted);
    bool ok=authorized_json("POST","/v1/profiles",body,response,sizeof response,&status);
    if(ok && (status==200 || status==201) && json_get(response,"id",remote,sizeof remote,false) && !strcmp(remote,id) &&
        json_get(response,"edition",edition,sizeof edition,false) && !strcmp(edition,ctx.edition) &&
        json_get(response,"label",remote_label,sizeof remote_label,false) && !strcmp(remote_label,label)) {
        profile_publish(&ctx,id,label,true,"This local profile is linked. Game saves stayed unchanged.");
        pthread_mutex_lock(&g_lock);
        if(ctx.generation==g_context_generation) { g_inbox_generation++; memset(&g_inbox,0,sizeof g_inbox); g_inbox.kind=-1; g_inbox.state=INBOX_IDLE; }
        pthread_mutex_unlock(&g_lock);
    } else profile_publish(&ctx,id,label,false,ok && status==409 ? "This profile could not be linked: the server rejected the binding." : "Could not link the profile. Retry will reuse its saved identity.");
done:
    CloseHandle(lock);
}
void account_inbox_request(int kind,int page) {
    if(kind<0 || kind>2 || page<0 || page>16666) return;
    pthread_mutex_lock(&g_lock);
    if(g_started && g_view.state==ACCOUNT_SIGNED_IN && !g_profile.busy && !claim_busy_locked() && !atomic_load(&g_cancel) &&
       (g_command==CMD_NONE || g_command==CMD_INBOX)) {
        g_inbox_generation++; memset(&g_inbox,0,sizeof g_inbox); g_inbox.kind=kind; g_inbox.page=page;
        g_inbox.state=INBOX_LOADING; g_command=CMD_INBOX;
    }
    pthread_mutex_unlock(&g_lock);
}
static bool inbox_parse(const char *data,AccountInboxSnapshot *out) {
    jsmn_parser parser; jsmntok_t tokens[1024]; jsmn_init(&parser);
    int count=jsmn_parse(&parser,data,strlen(data),tokens,1024),array=-1; bool has_more_seen=false;
    if(count<1 || tokens[0].type!=JSMN_OBJECT) return false;
    for(int i=1;i+1<count;i++) {
        if(tokens[i].parent!=0 || tokens[i].type!=JSMN_STRING) continue;
        int n=tokens[i].end-tokens[i].start;
        if(n==5 && !memcmp(data+tokens[i].start,"items",5)) { if(array>=0 || tokens[i+1].type!=JSMN_ARRAY) return false; array=i+1; }
        if(n==8 && !memcmp(data+tokens[i].start,"has_more",8)) {
            if(has_more_seen || tokens[i+1].type!=JSMN_PRIMITIVE) return false;
            has_more_seen=true; int len=tokens[i+1].end-tokens[i+1].start;
            if(len==4 && !memcmp(data+tokens[i+1].start,"true",4)) out->has_more=true;
            else if(len==5 && !memcmp(data+tokens[i+1].start,"false",5)) out->has_more=false;
            else return false;
        }
    }
    if(array<0 || !has_more_seen) return false;
    for(int i=array+1;i<count;i++) {
        if(tokens[i].parent!=array) continue;
        int len=tokens[i].end-tokens[i].start; char row[2048],number[16];
        if(tokens[i].type!=JSMN_OBJECT || len<=0 || len>=(int)sizeof row || out->count>=6) return false;
        memcpy(row,data+tokens[i].start,len); row[len]=0;
        AccountInboxRow *r=&out->rows[out->count];
        if(!json_get(row,"id",r->id,sizeof r->id,false) || !uuid_valid(r->id) ||
           !json_get(row,"title",r->title,sizeof r->title,false) || !r->title[0] ||
           !json_get(row,"status",r->status,sizeof r->status,false) ||
           strcmp(r->status,out->kind==0 ? "available" : out->kind==1 ? "pending" : "received") ||
           !json_get(row,"profile_label",r->profile_label,sizeof r->profile_label,false) ||
           !json_get(row,"item_count",number,sizeof number,true)) return false;
        long quantity=atol(number); if(quantity<1 || quantity>31968) return false; r->item_count=(int)quantity;
        // Only a presentation hint; no code or reward mutation is exposed by this read-only page.
        r->requires_code=strstr(row,"\"requires_code\":true") || strstr(row,"\"requires_code\": true");
        out->count++;
    }
    return true;
}
static void inbox_fetch(void) {
    AccountInboxSnapshot result={0}; unsigned generation; char profile[37],path[256],data[16384]; int status;
    pthread_mutex_lock(&g_lock);
    generation=g_inbox_generation; result.kind=g_inbox.kind; result.page=g_inbox.page;
    snprintf(profile,sizeof profile,"%s",g_profile.bound ? g_profile.id : "");
    pthread_mutex_unlock(&g_lock);
    if(result.kind<0 || result.kind>2) return;
    snprintf(path,sizeof path,"/v1/inbox?kind=%s&limit=6&offset=%d%s%s",result.kind==0 ? "available" : result.kind==1 ? "pending" : "received",
        result.page*6,profile[0] ? "&profile_id=" : "",profile);
    bool ok=authorized_json("GET",path,NULL,data,sizeof data,&status);
    result.state=ok && status==200 && inbox_parse(data,&result) ? INBOX_READY : INBOX_ERROR;
    if(result.state==INBOX_ERROR) { result.count=0; result.has_more=false; snprintf(result.detail,sizeof result.detail,"Could not load gifts. Retry when the account service is available."); }
    pthread_mutex_lock(&g_lock);
    if(generation==g_inbox_generation && g_view.state==ACCOUNT_SIGNED_IN && !atomic_load(&g_cancel)) g_inbox=result;
    pthread_mutex_unlock(&g_lock);
}
