# System fonts (optional)

Some of the game's text uses the font built into the Switch's system, which is not part of the game files. Without it,
Let's Go Recomp uses Windows' Yu Gothic (the same Japanese gothic style as the Switch's Morisawa UD Shin Go) and,
failing that, Arial. Most of the game draws its text with the fonts in its own romfs and is unaffected.

For the exact look, put a copy taken from **your** console here (system files *FontStandard*,
title 0100000000000811, and *FontNintendoExtension*, 0100000000000810):

- `nintendo_udsg-r_std_003.bfttf`: the main font (a `.ttf` with the same name, or spelled `nintendo_udsgr_std_003`,
  also works)
- `nintendo_ext_003.bfttf`: the button symbols (optional)

`LARECOMP_FONT=<file>` overrides all of this (.ttf / .otf / .ttc / .bfttf).

These fonts are Nintendo's property: they stay out of git (`.gitignore`) and must never be shared.
