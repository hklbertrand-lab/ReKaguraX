// Réglages du runtime (<jeu>.ini à côté de l'exécutable lancé depuis le dossier du jeu) et menu superposé (F1)
#pragma once
#include <atomic>
#include <cstdint>

struct Settings
{
    std::atomic<int> resolution{ 0 };     // 0 = 720p, 1 = 1280x800, 2 = 1080p, 3 = 1440p, 4 = 4K (rendu interne et fenêtre)
    std::atomic<bool> fullscreen{ false };
    std::atomic<bool> wide{ false };      // mode large expérimental : vue 3D élargie, panneaux latéraux masqués
    std::atomic<bool> fill{ true };       // écran plus étroit que 16:9 (16:10) : rogne les bords au lieu des bandes noires
    std::atomic<bool> showFps{ false };   // compteur d'images par seconde en haut à gauche
};
extern Settings g_settings;

void SettingsLoad();
void SettingsSave();
float SettingsScale();                     // facteur du rendu interne par rapport au 720p d'origine
void SettingsWindowSize(int& w, int& h);   // taille de fenêtre associée à la résolution
const char* SettingsResolutionName(int index);

// Menu (thread principal pour les événements, thread GPU pour le dessin)
bool MenuOpen();
bool MenuHandleKey(int scancode);          // true si la touche a été consommée
struct SDL_Gamepad;
void MenuHandlePad(int button, SDL_Gamepad* pad);   // bouton de manette enfoncé (L3+R3 : menu)
void MenuHandlePadAxis(int axis, int value);         // stick gauche dans le menu
bool MenuBlocksPad();       // true : ne pas transmettre la manette au jeu
// Dessine le menu dans des pixels BGRA (largeur x hauteur fixes) ; renvoie false si rien à afficher
bool MenuRender(uint32_t* pixels, int width, int height);
constexpr int kMenuWidth = 720, kMenuHeight = 298;
// Compteur d'images par seconde (même principe que le menu)
bool FpsRender(uint32_t* pixels, int width, int height, float fps);
constexpr int kFpsWidth = 176, kFpsHeight = 28;
void SettingsApplyWindow();               // applique plein écran et taille de fenêtre (thread principal)
