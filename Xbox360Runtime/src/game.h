// Ce que le runtime attend du jeu : défini une fois par jeu (game/game.cpp à côté du code recompilé)
#pragma once
#include <cstdint>

struct GameConfig
{
    const char* name;          // nom court (journaux, Vulkan, mémoire partagée)
    const char* windowTitle;   // titre de la fenêtre
    const char* iniFile;       // réglages, à côté de l'exécutable
    const char* envPrefix;     // préfixe des variables de débogage, ex. "IKARUGA_" -> IKARUGA_TIMING
    uint32_t entryPoint;       // 0 = point d'entrée du XEX
    bool wideField;            // mode large (hack propre à Ikaruga : zone de jeu verticale de 540 px)
};

extern const GameConfig g_game;

// getenv(préfixe du jeu + nom)
const char* GameEnv(const char* name);
