// Ikaruga : ce qui distingue ce jeu pour le runtime Xbox360Runtime
#include "game.h"

const GameConfig g_game{
    .name = "Ikaruga",
    .windowTitle = "Ikaruga  (F1 : réglages)",
    .iniFile = "ikaruga.ini",
    .envPrefix = "IKARUGA_",
    .entryPoint = 0x8217BA20,
    .wideField = true,
};

