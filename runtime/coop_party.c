#include "coop_party.h"
#include <stddef.h>
static uint16_t u16(const uint8_t *p) {return p[0]|(uint16_t)p[1]<<8;}
static uint32_t u32(const uint8_t *p) {return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
bool coop_party_pokemon_valid(const uint8_t data[COOP_PB7_BYTES]) {
    if(!data || (u16(data+4)&4))return false;
    uint32_t seed=u32(data);uint16_t sum=0;
    // The shuffled 4x56-byte core is encrypted by the native LCRNG. Addition
    // checks its checksum without needing to unshuffle or interpret its fields.
    for(unsigned i=8;i<232;i+=2){seed=seed*0x41c64e6du+0x6073u;sum+=(uint16_t)(u16(data+i)^(seed>>16));}
    if(sum!=u16(data+6))return false;
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
