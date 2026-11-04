// Réglages persistants et menu superposé. Le menu est dessiné par le moteur de rendu logiciel de SDL (police de
// débogage intégrée) dans une surface BGRA que vulkan.cpp copie par-dessus l'image présentée.
#include "settings.h"
#include <SDL3/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <mutex>

Settings g_settings;
extern SDL_Window* g_window;

static const char* kFile = "ikaruga.ini";
static const struct { const char* name; int w, h; } kResolutions[] = {
    { "1280x720 (d'origine)", 1280, 720 }, { "1280x800", 1280, 800 }, { "1920x1080", 1920, 1080 },
    { "2560x1440", 2560, 1440 }, { "3840x2160 (4K)", 3840, 2160 } };
static constexpr int kResolutionCount = int(sizeof(kResolutions) / sizeof(kResolutions[0]));
static int Res(int index) { return std::clamp(index, 0, kResolutionCount - 1); }

const char* SettingsResolutionName(int index) { return kResolutions[Res(index)].name; }

void SettingsWindowSize(int& w, int& h)
{
    w = kResolutions[Res(g_settings.resolution)].w;
    h = kResolutions[Res(g_settings.resolution)].h;
}

// Le jeu calcule en 720 lignes : le facteur suit la hauteur choisie
float SettingsScale() { return kResolutions[Res(g_settings.resolution)].h / 720.0f; }

static std::atomic<bool> s_open;

void SettingsLoad()
{
    if (getenv("IKARUGA_MENU")) s_open = true;   // tests : menu ouvert au démarrage
    FILE* f = fopen(kFile, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f))
    {
        int v;
        if (sscanf(line, "resolution = %d", &v) == 1)
        {
            // hauteur en pixels ; anciens fichiers : indice 0-3 de la liste sans 1080p
            static const int legacy[4] = { 0, 1, 3, 4 };
            if (v >= 0 && v < 4) g_settings.resolution = legacy[v];
            else for (int i = 0; i < kResolutionCount; i++) if (kResolutions[i].h == v) g_settings.resolution = i;
        }
        else if (sscanf(line, "fps = %d", &v) == 1) g_settings.showFps = v != 0;
        else if (sscanf(line, "fullscreen = %d", &v) == 1) g_settings.fullscreen = v != 0;
        else if (sscanf(line, "wide = %d", &v) == 1) g_settings.wide = v != 0;
        else if (sscanf(line, "fill = %d", &v) == 1) g_settings.fill = v != 0;
    }
    fclose(f);
}

void SettingsSave()
{
    FILE* f = fopen(kFile, "w");
    if (!f) return;
    fprintf(f, "# Ikaruga : réglages (F1 en jeu)\n# resolution : hauteur (720, 800, 1080, 1440, 2160)\n");
    fprintf(f, "resolution = %d\nfullscreen = %d\nwide = %d\nfill = %d\nfps = %d\n", kResolutions[Res(g_settings.resolution)].h,
        g_settings.fullscreen ? 1 : 0, g_settings.wide ? 1 : 0, g_settings.fill ? 1 : 0, g_settings.showFps ? 1 : 0);
    fclose(f);
}

// ---------------------------------------------------------------- menu

static std::atomic<int> s_selected;
enum { ITEM_RESOLUTION, ITEM_FULLSCREEN, ITEM_WIDE, ITEM_FILL, ITEM_FPS, ITEM_CLOSE, ITEM_COUNT };

bool MenuOpen() { return s_open; }

static void ApplyWindow()
{
    if (!g_window) return;
    SDL_SetWindowFullscreen(g_window, g_settings.fullscreen);
    if (!g_settings.fullscreen)
    {
        // Fenêtre à la taille choisie, réduite si l'écran est plus petit
        int w, h; SettingsWindowSize(w, h);
        SDL_Rect usable;
        if (SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(g_window), &usable))
        {
            float k = std::min(1.0f, std::min(float(usable.w) / w, float(usable.h - 40) / h));
            w = int(w * k); h = int(h * k);
        }
        SDL_SetWindowSize(g_window, w, h);
        SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
}

void SettingsApplyWindow() { ApplyWindow(); }

static void Change(int item, int dir)
{
    switch (item)
    {
    case ITEM_RESOLUTION: g_settings.resolution = (Res(g_settings.resolution) + dir + kResolutionCount) % kResolutionCount; ApplyWindow(); break;
    case ITEM_FULLSCREEN: g_settings.fullscreen = !g_settings.fullscreen; ApplyWindow(); break;
    case ITEM_WIDE: g_settings.wide = !g_settings.wide; break;
    case ITEM_FILL: g_settings.fill = !g_settings.fill; break;
    case ITEM_FPS: g_settings.showFps = !g_settings.showFps; break;
    case ITEM_CLOSE: s_open = false; break;
    }
    SettingsSave();
}

bool MenuHandleKey(int scancode)
{
    if (scancode == SDL_SCANCODE_F1) { s_open = !s_open; return true; }
    if (scancode == SDL_SCANCODE_F9 || scancode == SDL_SCANCODE_F10)
    {
        // Débogage : F9 coupe/rétablit le test de profondeur, F10 décrit l'image suivante dans le journal
        extern std::atomic<uint64_t> g_dumpFrame; extern std::atomic<bool> g_debugNoDepth; extern uint64_t g_presentCount;
        if (scancode == SDL_SCANCODE_F9) g_debugNoDepth = !g_debugNoDepth;
        else g_dumpFrame = g_presentCount + 2;
        fprintf(stderr, "[k] débogage : %s\n", scancode == SDL_SCANCODE_F9 ? (g_debugNoDepth ? "profondeur coupée" : "profondeur rétablie") : "image suivante décrite");
        return true;
    }
    if (scancode == SDL_SCANCODE_F11) { g_settings.fullscreen = !g_settings.fullscreen; ApplyWindow(); SettingsSave(); return true; }
    if (!s_open) return false;
    switch (scancode)
    {
    case SDL_SCANCODE_UP: s_selected = (s_selected + ITEM_COUNT - 1) % ITEM_COUNT; break;
    case SDL_SCANCODE_DOWN: s_selected = (s_selected + 1) % ITEM_COUNT; break;
    case SDL_SCANCODE_LEFT: Change(s_selected, -1); break;
    case SDL_SCANCODE_RIGHT: case SDL_SCANCODE_RETURN: Change(s_selected, 1); break;
    case SDL_SCANCODE_ESCAPE: s_open = false; break;
    default: break;
    }
    return true;   // menu ouvert : aucune touche ne part au jeu
}

// Manette : L3+R3 ouvre/ferme le menu ; croix ou stick gauche pour naviguer, A valide, B ferme
// Menu fermé à la manette : entrées ignorées encore un court instant, pour que le bouton qui ferme ne parte pas au
// jeu. Délai fixe et non "jusqu'au relâchement" : à deux joueurs, un tir maintenu en continu bloquait les manettes.
static std::atomic<uint64_t> s_padHoldUntil;
static void HoldPad() { s_padHoldUntil = SDL_GetTicks() + 300; }

void MenuHandlePad(int button, SDL_Gamepad* pad)
{
    if ((button == SDL_GAMEPAD_BUTTON_LEFT_STICK && pad && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_STICK)) ||
        (button == SDL_GAMEPAD_BUTTON_RIGHT_STICK && pad && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_STICK)))
    {
        s_open = !s_open;
        HoldPad();
        return;
    }
    if (!s_open) return;
    switch (button)
    {
    case SDL_GAMEPAD_BUTTON_DPAD_UP: MenuHandleKey(SDL_SCANCODE_UP); break;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: MenuHandleKey(SDL_SCANCODE_DOWN); break;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: MenuHandleKey(SDL_SCANCODE_LEFT); break;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: case SDL_GAMEPAD_BUTTON_SOUTH: MenuHandleKey(SDL_SCANCODE_RIGHT); break;
    case SDL_GAMEPAD_BUTTON_EAST: case SDL_GAMEPAD_BUTTON_START: MenuHandleKey(SDL_SCANCODE_ESCAPE); break;
    default: break;
    }
    if (!s_open) HoldPad();
}

// Stick gauche : une impulsion par poussée franche (seuil haut), réarmée près du centre (seuil bas)
void MenuHandlePadAxis(int axis, int value)
{
    static int held[2];   // -1, 0, +1 par axe X / Y
    int i = axis == SDL_GAMEPAD_AXIS_LEFTX ? 0 : axis == SDL_GAMEPAD_AXIS_LEFTY ? 1 : -1;
    if (i < 0) return;
    int dir = value > 20000 ? 1 : value < -20000 ? -1 : (std::abs(value) < 8000 ? 0 : held[i]);
    if (dir == held[i]) return;
    held[i] = dir;
    if (!dir || !s_open) return;
    if (i == 0) MenuHandleKey(dir > 0 ? SDL_SCANCODE_RIGHT : SDL_SCANCODE_LEFT);
    else MenuHandleKey(dir > 0 ? SDL_SCANCODE_DOWN : SDL_SCANCODE_UP);   // Y positif vers le bas dans SDL
    if (!s_open) HoldPad();
}

// Le jeu ne voit pas la manette pendant le menu, ni dans les 300 ms qui suivent sa fermeture
bool MenuBlocksPad()
{
    return s_open || SDL_GetTicks() < s_padHoldUntil;
}

bool MenuRender(uint32_t* pixels, int width, int height)
{
    if (!s_open) return false;
    SDL_Surface* surface = SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_ARGB8888, pixels, width * 4);   // octets B,G,R,A
    if (!surface) return false;
    SDL_Renderer* r = SDL_CreateSoftwareRenderer(surface);
    if (!r) { SDL_DestroySurface(surface); return false; }
    SDL_SetRenderDrawColor(r, 12, 14, 24, 255);
    SDL_RenderClear(r);
    SDL_SetRenderDrawColor(r, 200, 160, 90, 255);
    SDL_FRect border{ 1, 1, float(width - 2), float(height - 2) };
    SDL_RenderRect(r, &border);
    SDL_SetRenderScale(r, 2, 2);
    SDL_SetRenderDrawColor(r, 240, 200, 120, 255);
    SDL_RenderDebugText(r, 12, 8, "IKARUGA - REGLAGES");
    char lines[ITEM_COUNT][64];
    snprintf(lines[ITEM_RESOLUTION], 64, "Resolution   < %s >", SettingsResolutionName(g_settings.resolution));
    snprintf(lines[ITEM_FULLSCREEN], 64, "Plein ecran  < %s >", g_settings.fullscreen ? "oui" : "non");
    snprintf(lines[ITEM_WIDE], 64, "Mode large   < %s >", g_settings.wide ? "oui (exp.)" : "non");
    snprintf(lines[ITEM_FILL], 64, "Cadrage      < %s >", g_settings.fill ? "remplir (16:10)" : "bandes noires");
    snprintf(lines[ITEM_FPS], 64, "Afficher FPS < %s >", g_settings.showFps ? "oui" : "non");
    snprintf(lines[ITEM_CLOSE], 64, "Fermer");
    for (int i = 0; i < ITEM_COUNT; i++)
    {
        float y = 30.0f + i * 14.0f;
        if (i == s_selected)
        {
            SDL_SetRenderDrawColor(r, 40, 60, 140, 255);
            SDL_FRect bar{ 6, y - 3, float(width / 2 - 12), 13 };
            SDL_RenderFillRect(r, &bar);
        }
        SDL_SetRenderDrawColor(r, 235, 235, 235, 255);
        SDL_RenderDebugText(r, 12, y, lines[i]);
    }
    SDL_SetRenderDrawColor(r, 140, 140, 150, 255);
    SDL_RenderDebugText(r, 12, 116, "Fleches : choisir/changer  Echap : fermer");
    SDL_RenderDebugText(r, 12, 126, "F1 : menu   F11 / Alt+Entree : plein ecran");
    SDL_RenderDebugText(r, 12, 136,"Manette: L3+R3 menu  A changer  B fermer");
    SDL_RenderPresent(r);
    SDL_DestroyRenderer(r);
    SDL_DestroySurface(surface);
    return true;
}

bool FpsRender(uint32_t* pixels, int width, int height, float fps)
{
    if (!g_settings.showFps) return false;
    SDL_Surface* surface = SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_ARGB8888, pixels, width * 4);
    if (!surface) return false;
    SDL_Renderer* r = SDL_CreateSoftwareRenderer(surface);
    if (!r) { SDL_DestroySurface(surface); return false; }
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);
    SDL_SetRenderScale(r, 2, 2);
    // vert à 60, jaune au-dessous de 58, rouge au-dessous de 50
    if (fps >= 58) SDL_SetRenderDrawColor(r, 120, 230, 120, 255);
    else if (fps >= 50) SDL_SetRenderDrawColor(r, 240, 210, 90, 255);
    else SDL_SetRenderDrawColor(r, 240, 90, 80, 255);
    char text[32];
    snprintf(text, sizeof text, "%5.1f FPS", fps);
    SDL_RenderDebugText(r, 6, 3, text);
    SDL_RenderPresent(r);
    SDL_DestroyRenderer(r);
    SDL_DestroySurface(surface);
    return true;
}
