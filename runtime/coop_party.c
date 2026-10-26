#include "coop_party.h"
#include <stddef.h>
static uint16_t u16(const uint8_t *p) {return p[0]|(uint16_t)p[1]<<8;}
static uint32_t u32(const uint8_t *p) {return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
bool coop_party_pokemon_valid(const uint8_t data[COOP_PB7_BYTES]) {
    if(!data || (u16(data+4)&4))return false;
    uint32_t seed=u32(data);uint16_t sum=0;uint8_t core[224];
    // Check the native encrypted core before any data reaches the importer.
    for(unsigned i=8;i<232;i+=2){
        seed=seed*0x41c64e6du+0x6073u;uint16_t v=u16(data+i)^(seed>>16);
        sum+=v;core[i-8]=v;core[i-7]=v>>8;
    }
    if(sum!=u16(data+6))return false;
    // Physical blocks follow the lexicographic permutation selected by EC.
    // Derive the inverse with factorial digits instead of a shuffle table.
    unsigned rank=((u32(data)>>13)&31)%24,remaining[4]={0,1,2,3},physical[4];
    const unsigned factorial[4]={6,2,1,1};
    for(unsigned slot=0;slot<4;slot++){
        unsigned digit=rank/factorial[slot];rank%=factorial[slot];
        physical[remaining[digit]]=slot;
        for(unsigned j=digit;j<3-slot;j++)remaining[j]=remaining[j+1];
    }
    unsigned species=u16(core+physical[0]*56);
    if(!species || species>809)return false;
    const uint8_t *moves=core+physical[1]*56+26;
    for(unsigned i=0;i<4;i++){
        if(u16(moves+2*i)>742 || moves[8+i]>64 || moves[12+i]>3 || u16(moves+16+2*i)>742)return false;
    }
    uint8_t calc[28];seed=u32(data);
    for(unsigned i=0;i<28;i+=2){seed=seed*0x41c64e6du+0x6073u;uint16_t v=u16(data+232+i)^(seed>>16);calc[i]=v;calc[i+1]=v>>8;}
    unsigned level=calc[4],hp=u16(calc+8),maxhp=u16(calc+10);
    if(!level || level>100 || !maxhp || maxhp>999 || hp>maxhp)return false;
    for(unsigned i=12;i<=20;i+=2)if(!u16(calc+i) || u16(calc+i)>999)return false;
    return true;
}
bool coop_party_valid(const CoopParty *party) {
    if(!party || !party->count || party->count>COOP_PARTY_MAX)return false;
    for(unsigned i=0;i<party->count;i++)if(!coop_party_pokemon_valid(party->pokemon[i]))return false;
    return true;
}
uint64_t coop_party_digest(const CoopParty *party) {
    if(!party || party->count>COOP_PARTY_MAX)return 0;
    uint64_t h=(0xcbf29ce484222645ull^party->count)*0x100000001b3ull;
    for(unsigned i=0;i<party->count;i++)for(unsigned j=0;j<COOP_PB7_BYTES;j++)h=(h^party->pokemon[i][j])*0x100000001b3ull;
    return h;
}
