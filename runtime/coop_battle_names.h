#pragma once
void coop_battle_names_load(void); // game thread, after RomFS initialization
const char *coop_battle_move_name(unsigned id);
const char *coop_battle_species_name(unsigned id);
