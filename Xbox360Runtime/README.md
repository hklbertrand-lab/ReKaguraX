# Xbox360Runtime

Runtime commun aux jeux Xbox 360 recompilés avec XenonRecomp/XenosRecomp. Sorti d'Ikaruga (ReKaguraX).

- `src/` : noyau xboxkrnl/xam émulé, threads et interruptions, GPU Xenos -> Vulkan, audio XMA, réglages (F1), main.
- `src/game.h` : ce que chaque jeu doit définir (`g_game` : nom, titre, ini, préfixe des variables de débogage,
  point d'entrée, options propres au jeu).
- `runtime.cmake` : `x360_add_game(<cible> GAME_DIR <dossier> SOURCES <fichiers du jeu>)`.
- `tools/gen_stubs.py` : génère `imports_stub.cpp` (imports noyau pas encore implémentés) pour un jeu.
- `patches/` : correctifs appliqués à XenonRecomp et XenosRecomp.

Un jeu fournit : `game/game.cpp` (g_game), `game/imports_stub.cpp`, ses éventuels hooks de débogage, sa config
XenonRecomp, et le code généré dans `ppc/` et `build/shaders/`. Exemple complet : `../Ikaruga`.

Variables de débogage : `<PRÉFIXE>_LOGFILE`, `_TIMING`, `_PROFILE`, `_PROFILE_AT`, `_SPINSLEEP`, `_PASSLOG`, etc.
(préfixe `IKARUGA_` pour Ikaruga).
