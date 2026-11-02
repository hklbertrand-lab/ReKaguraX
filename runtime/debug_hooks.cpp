// Points d'arrêt de débogage : redéfinir un sub_ (symbole faible) pour tracer ses arguments
#include "guest.h"
#include <initializer_list>
#include <vector>
#include <chrono>
#ifdef _WIN32
#include <windows.h>
static void DumpHostStack()
{
    void* frames[16]; USHORT n = CaptureStackBackTrace(0, 16, frames, nullptr);
    for (USHORT i = 0; i < n; i++)
        fprintf(stderr, "[watch]   pile exe+%llX\n",(unsigned long long)((uint8_t*)frames[i] - (uint8_t*)GetModuleHandleW(nullptr)));
}
#else
static void DumpHostStack() {}
#endif

extern "C" PPC_FUNC(__imp__sub_82174280);
PPC_FUNC(sub_82174280)
{
    KLOG("sub_82174280(r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X r9=%08X r10=%08X) lr-ish",
        ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32);
    __imp__sub_82174280(ctx, base);
}

extern "C" PPC_FUNC(__imp__sub_82172CA8);
PPC_FUNC(sub_82172CA8)
{
    uint32_t in = ctx.r6.u32, out = ctx.r7.u32;
    KLOG("sub_82172CA8(r4=%08X r5=%08X) in@%08X: %08X %08X %08X %08X %08X %08X", ctx.r4.u32, ctx.r5.u32, in,
        Rd32(in), Rd32(in + 4), Rd32(in + 8), Rd32(in + 12), Rd32(in + 16), Rd32(in + 20));
    __imp__sub_82172CA8(ctx, base);
    KLOG("  out: %08X %08X %08X %08X %08X %08X", Rd32(out), Rd32(out + 4), Rd32(out + 8), Rd32(out + 12), Rd32(out + 16), Rd32(out + 20));
}

extern "C" PPC_FUNC(__imp__sub_82167938);
PPC_FUNC(sub_82167938)
{
    KLOG("sub_82167938 (création device) r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32);
    for (uint32_t r : { ctx.r4.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32 })
        if (r > 0x10000 && r < 0xC0000000)
        {
            char line[400]; int n = 0;
            for (int i = 0; i < 124; i += 4) n += snprintf(line + n, sizeof line - n, "%08X ", Rd32(r + i));
            KLOG("  [%08X] %s", r, line);
        }
    __imp__sub_82167938(ctx, base);
}



extern "C" PPC_FUNC(__imp__sub_8216F760);
PPC_FUNC(sub_8216F760)
{
    uint32_t t = ctx.r4.u32;
    static int c;
    if (c++ < 3)
    {
        char line[400]; int n = 0;
        for (int i = 0; i < 0x34; i += 4) n += snprintf(line + n, sizeof line - n, "%08X ", Rd32(t + i));
        KLOG("sub_8216F760 texture %08X : %s", t, line);
    }
    __imp__sub_8216F760(ctx, base);
}

extern uint32_t g_fenceTarget;
extern "C" PPC_FUNC(__imp__sub_821600C8);
PPC_FUNC(sub_821600C8)
{
    static int c;
    uint32_t dev = Rd32(ctx.r3.u32);
    static auto last = std::chrono::steady_clock::now();
    if (std::chrono::steady_clock::now() - last > std::chrono::seconds(3))
    {
        last = std::chrono::steady_clock::now();
        KLOG("attente GPU (retour %08X) : dev=%08X fence=%08X dernière=%08X wptr=%08X rptr=%08X +13232=%08X écriture D3D=%08X +52=%08X +56=%08X cible=%08X", uint32_t(ctx.lr),
            dev, Rd32(Rd32(dev + 10896)), Rd32(dev + 10908), Rd32(0x7FC80714), Rd32(0xA020A03C), Rd32(dev + 13232), Rd32(dev + 48), Rd32(dev + 52), Rd32(dev + 56), g_fenceTarget);
        for (uint32_t w : { 0x40004F5Cu, 0x40004FACu })
        {
            uint32_t sh = Rd32(w);
            KLOG("  travailleur %08X cpu=%u file=%u/%u | partagé %08X verrou=%u masque=%08X compteur=%u +44=%u +96=%u +100=%u +24=%u +28=%u +32=%08X +36=%08X", w, Rd32(w + 4), Rd32(w + 60), Rd32(w + 56),
                sh, Rd32(sh), Rd32(sh + 56), Rd32(sh + 60), Rd32(sh + 44), Rd32(sh + 96), Rd32(sh + 100), Rd32(sh + 24), Rd32(sh + 28), Rd32(sh + 32), Rd32(sh + 36));
        }
    }
    __imp__sub_821600C8(ctx, base);
}



// Écran ESRB : état de l'objet dessiné par sub_82102080
extern "C" PPC_FUNC(__imp__sub_82102080);
PPC_FUNC(sub_82102080)
{
    static int n;
    uint32_t o = ctx.r3.u32;
    if (getenv("IKARUGA_ESRBLOG") && n++ % 60 == 0)
    {
        float f64; uint32_t v = Rd32(o + 64); memcpy(&f64, &v, 4);
        KLOG("ESRB obj=%08X étape48=%u o49=%u état=%u,%u,%u,%u +64=%.3f +68=%08X +40=%08X +44=%08X", o, Rd8(o + 48), Rd8(o + 49), Rd8(o + 50), Rd8(o + 51), Rd8(o + 52), Rd8(o + 53),
            f64, Rd32(o + 68), Rd32(o + 40), Rd32(o + 44));
    }
    if (getenv("IKARUGA_ESRBLOG") && n % 60 == 1)
    {
        uint32_t g = Rd32(0x827C0CD4), sys = Rd32(g + 2596);
        KLOG("  sous-système [g+2596]=%08X vtable=%08X occupé(+112)=%u +100..+120: %08X %08X %08X %08X %08X %08X", sys, Rd32(sys), Rd8(sys + 112),
            Rd32(sys + 100), Rd32(sys + 104), Rd32(sys + 108), Rd32(sys + 112), Rd32(sys + 116), Rd32(sys + 120));
    }
    __imp__sub_82102080(ctx, base);
}

// Attente de fence GPU de D3D : cible, dernière fence émise, valeur écrite par le GPU
extern "C" PPC_FUNC(__imp__sub_82161A20);
uint32_t g_fenceTarget;
PPC_FUNC(sub_82161A20)
{
    g_fenceTarget = ctx.r4.u32;
    static int n;
    uint32_t dev = ctx.r3.u32;
    static auto last = std::chrono::steady_clock::now();
    if (getenv("IKARUGA_FENCELOG") && std::chrono::steady_clock::now() - last > std::chrono::seconds(2) && (last = std::chrono::steady_clock::now(), true))
        KLOG("attente fence : cible=%08X dernière=%08X gpu=%08X +13232=%08X r5=%08X", ctx.r4.u32, Rd32(dev + 10908),
            Rd32(Rd32(dev + 10896)), Rd32(dev + 13232), ctx.r5.u32);
    __imp__sub_82161A20(ctx, base);
}

// Processeur de commandes D3D des threads de travail : état du verrou partagé à l'entrée et à la sortie
extern "C" PPC_FUNC(__imp__sub_82170B28);
PPC_FUNC(sub_82170B28)
{
    static bool on = getenv("IKARUGA_LOCKLOG");
    uint32_t w = ctx.r3.u32, sh = Rd32(w);
    uint32_t cpu = Rd8(ctx.r13.u32 + 0x10C);
    if (on) KLOG("82170B28 entrée cpu%u ctx=%08X verrou=%u masque=%08X compteur=%u file=%u/%u", cpu, w, Rd32(sh), Rd32(sh + 56), Rd32(sh + 60), Rd32(w + 60), Rd32(w + 56));
    __imp__sub_82170B28(ctx, base);
    if (on) KLOG("82170B28 sortie  cpu%u ctx=%08X verrou=%u masque=%08X compteur=%u r3=%u", cpu, w, Rd32(sh), Rd32(sh + 56), Rd32(sh + 60), ctx.r3.u32);
}
