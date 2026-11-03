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
    { "1280x720 (d'origine)", 1280, 720 }, { "1280x800", 1280, 800 }, { "2560x1440", 2560, 1440 }, { "3840x2160 (4K)", 3840, 2160 } };

const char* SettingsResolutionName(int index) { return kResolutions[index & 3].name; }

void SettingsWindowSize(int& w, int& h)
{
    w = kResolutions[g_settings.resolution & 3].w;
    h = kResolutions[g_settings.resolution & 3].h;
}

// Le jeu calcule en 720 lignes : le facteur suit la hauteur choisie
float SettingsScale() { return kResolutions[g_settings.resolution & 3].h / 720.0f; }

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
        if (sscanf(line, "resolution = %d", &v) == 1) g_settings.resolution = v & 3;
        else if (sscanf(line, "fullscreen = %d", &v) == 1) g_settings.fullscreen = v != 0;
        else if (sscanf(line, "wide = %d", &v) == 1) g_settings.wide = v != 0;
    }
    fclose(f);
}

void SettingsSave()
{
    FILE* f = fopen(kFile, "w");
    if (!f) return;
    fprintf(f, "# Ikaruga : réglages (F1 en jeu)\n# resolution : 0 = 1280x720, 1 = 1280x800, 2 = 2560x1440, 3 = 3840x2160\n");
    fprintf(f, "resolution = %d\nfullscreen = %d\nwide = %d\n", g_settings.resolution.load(), g_settings.fullscreen ? 1 : 0, g_settings.wide ? 1 : 0);
    fclose(f);
}

// ---------------------------------------------------------------- menu

static std::atomic<int> s_selected;
enum { ITEM_RESOLUTION, ITEM_FULLSCREEN, ITEM_WIDE, ITEM_CLOSE, ITEM_COUNT };

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
    case ITEM_RESOLUTION: g_settings.resolution = (g_settings.resolution + dir + 4) & 3; ApplyWindow(); break;
    case ITEM_FULLSCREEN: g_settings.fullscreen = !g_settings.fullscreen; ApplyWindow(); break;
    case ITEM_WIDE: g_settings.wide = !g_settings.wide; break;
    case ITEM_CLOSE: s_open = false; break;
    }
    SettingsSave();
}

bool MenuHandleKey(int scancode)
{
    if (scancode == SDL_SCANCODE_F1) { s_open = !s_open; return true; }
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
    SDL_RenderDebugText(r, 12, 94, "Fleches : choisir/changer  Echap : fermer");
    SDL_RenderDebugText(r, 12, 104, "F1 : menu   F11 / Alt+Entree : plein ecran");
    SDL_RenderPresent(r);
    SDL_DestroyRenderer(r);
    SDL_DestroySurface(surface);
    return true;
}
