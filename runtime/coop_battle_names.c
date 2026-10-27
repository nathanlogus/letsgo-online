// Read names from the user's game image; no copied text assets are shipped.
#include "coop_battle_names.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <unistd.h>
#ifdef _WIN32
#include "platform/win_compat.h"
#endif
extern bool romfs_image_file(const char *,int *,uint64_t *,uint64_t *);
static char moves[743][64],species[810][64],trainers[1024][64];
static atomic_bool ready;
static uint16_t u16(const uint8_t *p){return p[0]|(uint16_t)p[1]<<8;}
static uint32_t u32(const uint8_t *p){return u16(p)|(uint32_t)u16(p+2)<<16;}
static void load(const char *path,char (*names)[64],unsigned capacity){
    int fd;uint64_t off,size;
    if(!romfs_image_file(path,&fd,&off,&size) || size<16 || size>1048576)return;
    uint8_t *data=malloc(size);if(!data)return;
    if(pread(fd,data,size,off)!=(ssize_t)size){free(data);return;}
    unsigned n=u16(data+2),section=u32(data+12);
    if(n>capacity || section>size || n*8+4>size-section){free(data);return;}
    for(unsigned i=0;i<n;i++){
        const uint8_t *entry=data+section+4+i*8;unsigned at=u32(entry),count=u16(entry+4);
        if(at>size-section || count*2>size-section-at)continue;
        uint16_t key=(uint16_t)(0x7c89+i*0x2983);unsigned out=0;
        for(unsigned k=0;k<count && out<60;k++){
            uint16_t c=u16(data+section+at+2*k)^key;key=(uint16_t)((key<<3)|(key>>13));
            if(!c || c==0xffff)break;
            if(c<32 || c>=0xd800)continue;
            if(c<128)names[i][out++]=c;
            else if(c<2048){names[i][out++]=0xc0|(c>>6);names[i][out++]=0x80|(c&63);}
            else {names[i][out++]=0xe0|(c>>12);names[i][out++]=0x80|((c>>6)&63);names[i][out++]=0x80|(c&63);}
        }
        names[i][out]=0;
    }
    free(data);
}
void coop_battle_names_load(void){
    if(atomic_load_explicit(&ready,memory_order_acquire))return;
    load("bin/message/English/common/wazaname.dat",moves,743);
    load("bin/message/English/common/monsname.dat",species,810);
    load("bin/message/English/common/trname.dat",trainers,1024);
    atomic_store_explicit(&ready,true,memory_order_release);
}
const char *coop_battle_move_name(unsigned id){return atomic_load_explicit(&ready,memory_order_acquire) && id<743 && moves[id][0] ? moves[id] : "Move";}
const char *coop_battle_trainer_name(unsigned id){return atomic_load_explicit(&ready,memory_order_acquire) && id<1024 && trainers[id][0] ? trainers[id] : "a trainer";}
const char *coop_battle_species_name(unsigned id){return atomic_load_explicit(&ready,memory_order_acquire) && id<810 && species[id][0] ? species[id] : "Opponent";}
