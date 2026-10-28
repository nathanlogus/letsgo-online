#pragma once
// game thread, after RomFS initialization; lang: the game's message language (0 JPN ... 2 English ... 9 Trad_Chinese)
void coop_battle_names_load(unsigned lang);
const char *coop_battle_move_name(unsigned id);
const char *coop_battle_species_name(unsigned id);
const char *coop_battle_trainer_name(unsigned id); // trainer index (trname.dat); "a trainer" without one
