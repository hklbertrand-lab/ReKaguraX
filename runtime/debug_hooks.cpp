// Points d'arrêt de débogage : redéfinir un sub_ (symbole faible) pour tracer ses arguments
#include "guest.h"
#include <initializer_list>
#include <vector>
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

// Vérifie le memcpy recompilé du jeu contre une copie hôte
extern "C" PPC_FUNC(__imp__sub_821E0340);
PPC_FUNC(sub_821E0340)
{
    uint32_t dst = ctx.r3.u32, src = ctx.r4.u32, n = ctx.r5.u32;
    std::vector<uint8_t> expect(g_base + src, g_base + src + n);
    __imp__sub_821E0340(ctx, base);
    if (memcmp(expect.data(), g_base + dst, n) != 0)
    {
        static int count;
        if (count++ < 10)
        {
            uint32_t i = 0; while (expect[i] == g_base[dst + i]) i++;
            KLOG("memcpy FAUX dst=%08X src=%08X n=%u : premier écart à +%u", dst, src, n, i);
        }
    }
}

extern "C" PPC_FUNC(__imp__sub_821E0870);
PPC_FUNC(sub_821E0870)
{
    uint32_t dst = ctx.r3.u32, v = ctx.r4.u32 & 0xFF, n = ctx.r5.u32;
    __imp__sub_821E0870(ctx, base);
    for (uint32_t i = 0; i < n; i++)
        if (g_base[dst + i] != v) { static int c; if (c++ < 10) KLOG("memset FAUX dst=%08X v=%02X n=%u écart +%u", dst, v, n, i); break; }
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

extern "C" PPC_FUNC(__imp__sub_821600C8);
PPC_FUNC(sub_821600C8)
{
    static int c;
    uint32_t dev = Rd32(ctx.r3.u32);
    if (c++ % 200000 == 0)
        KLOG("attente GPU : dev=%08X [10896]=%08X -> %08X, wptr reg=%08X, wb[0x20A03C]=%08X, 10888=%08X",
            dev, Rd32(dev + 10896), Rd32(Rd32(dev + 10896)), Rd32(0x7FC80714), Rd32(0xA020A03C), Rd32(dev + 10888));
    __imp__sub_821600C8(ctx, base);
}

// libjpeg (jpeg_consume_input ?) : état et gestionnaire d'erreur
extern "C" PPC_FUNC(__imp__sub_822ACBD8);
PPC_FUNC(sub_822ACBD8)
{
    uint32_t cinfo = ctx.r3.u32, err = Rd32(cinfo);
    static int c;
    if (c++ < 5)
        KLOG("jpeg cinfo=%08X state=%u err=%08X error_exit=%08X emit=%08X src=%08X", cinfo, Rd32(cinfo + 420), err,
            err ? Rd32(err) : 0, err ? Rd32(err + 4) : 0, Rd32(cinfo + 24));
    if (c == 1) DumpHostStack();
    __imp__sub_822ACBD8(ctx, base);
    if (err && c < 6) KLOG("  -> msg_code=%u", Rd32(err + 20));
}

// error_exit de libjpeg : affiche le code d'erreur (msg_code à err+20)
extern "C" PPC_FUNC(__imp__sub_82247648);
PPC_FUNC(sub_82247648)
{
    uint32_t cinfo = ctx.r3.u32, err = Rd32(cinfo);
    KLOG("libjpeg error_exit : msg_code=%u parm=%u,%u", Rd32(err + 20), Rd32(err + 24), Rd32(err + 28));
    DumpHostStack();
    __imp__sub_82247648(ctx, base);
}
