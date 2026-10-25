#pragma once
#include "../cpu.h"
#include "../coop_party.h"
bool coop_battle_capture_party(Cpu *c,CoopParty *party);
bool coop_battle_check_import(Cpu *c,const CoopParty *party,uint16_t species[COOP_PARTY_MAX]);
void coop_battle_field(Cpu *c);
void coop_battle_configure(int build);
void coop_battle_install_hooks(void);
