// Processeur de commandes GPU Xenos (PM4) minimal : registres, écritures mémoire, fences, interruptions.
// Pas encore de rendu : les dessins sont ignorés. Sémantique des paquets reprise de Xenia (command_processor.cc).
#include "guest.h"
#include "shaders.h"
#include "render.h"
#include <atomic>
#include <chrono>
#include <immintrin.h>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

static uint32_t s_regs[0x8000];
static uint32_t s_ringBase, s_ringSize, s_rptr, s_rptrWriteBack;
uint32_t g_gpuRptr, g_gpuLastWptr;   // débogage
static uint32_t s_swapCounter;
static uint32_t s_interruptCallback, s_interruptCallbackCtx;
static PPCContext s_ctx;   // contexte invité pour appeler le callback d'interruption depuis le thread GPU

enum : uint32_t
{
    REG_SCRATCH_UMSK = 0x01DC, REG_SCRATCH_ADDR = 0x01DD, REG_SCRATCH_REG0 = 0x0578,
    REG_COHER_STATUS_HOST = 0x0A31,
    MMIO_BASE = 0x7FC80000, REG_CP_RB_WPTR = 0x01C5,
};

// Adresse physique GPU -> adresse invitée (vue 0xA0000000 de la mémoire physique)
static uint32_t Phys(uint32_t p) { return 0xA0000000 | (p & 0x1FFFFFFF); }

static uint32_t GpuSwap(uint32_t v, uint32_t endian)
{
    switch (endian & 3)
    {
    case 1: return ((v & 0x00FF00FF) << 8) | ((v >> 8) & 0x00FF00FF);   // 8in16
    case 2: return __builtin_bswap32(v);                              // 8in32
    case 3: return (v << 16) | (v >> 16);                             // 16in32
    default: return v;
    }
}

// Le GPU lit la mémoire brute (octets big-endian vus en little-endian) puis applique le swap demandé
// dans les 2 bits bas de l'adresse ; Rd32/Wr32 font déjà un swap, d'où le bswap supplémentaire.
static uint32_t s_curOpcode;
static void MemWrite(uint32_t addr, uint32_t value)
{
    Wr32(Phys(addr & ~3u), __builtin_bswap32(GpuSwap(value, addr & 3)));
    static int n;
    static bool ibLog = getenv("IKARUGA_IBLOG");
    if ((addr & 0x1FFFFFFC) == 0x20A000 && (ibLog || (getenv("IKARUGA_FENCELOG") && (n++ % 50) == 0)))
        KLOG("fence écrite par opcode %02X : %08X", s_curOpcode, Rd32(Phys(addr & ~3u)));
}
static uint32_t MemRead(uint32_t addr) { return GpuSwap(__builtin_bswap32(Rd32(Phys(addr & ~3u))), addr & 3); }

// IKARUGA_TIMING : temps d'attente cumulés du thread GPU (µs) : WAIT_REG_MEM, interruptions, fence Vulkan, présentation
std::atomic<uint64_t> g_gpuWaitUs[4];
struct WaitTimer
{
    int slot; std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    ~WaitTimer() { g_gpuWaitUs[slot] += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count(); }
};

std::recursive_mutex g_interruptMutex;
std::recursive_mutex& GuestCpuLock(uint32_t cpu);
bool InterruptsDeliver(uint32_t cpu, uint32_t callback, uint32_t source, uint32_t arg);

void GpuDispatchInterrupt(uint32_t source, uint32_t cpu)
{
    if (!s_interruptCallback) return;
    WaitTimer timer{ 1 };
    std::lock_guard lock(g_interruptMutex);   // le matériel sérialise les interruptions graphiques
    if (InterruptsDeliver(cpu, s_interruptCallback, source, s_interruptCallbackCtx)) return;   // exécutée sur le CPU visé
    std::lock_guard cpuLock(GuestCpuLock(cpu));     // repli : pas pendant une section "EE = 0" de ce CPU
    Wr8(s_ctx.r13.u32 + 0x10C, (uint8_t)cpu);   // processeur courant vu par le gestionnaire
    s_ctx.r3.u64 = source;
    s_ctx.r4.u64 = s_interruptCallbackCtx;
    GuestCall(s_ctx, s_interruptCallback);
}

static void WriteRegister(uint32_t index, uint32_t value)
{
    if (index >= 0x8000) return;
    s_regs[index] = value;
    // Le CPU lit les registres GPU via la fenêtre MMIO (ex. D3D lit dans SCRATCH 0x57C la fonction à
    // appeler lors d'une interruption) : on y reflète chaque écriture du flux de commandes
    if (index != REG_CP_RB_WPTR) Wr32(MMIO_BASE + index * 4, value);
    if (index >= REG_SCRATCH_REG0 && index < REG_SCRATCH_REG0 + 8)
    {
        uint32_t n = index - REG_SCRATCH_REG0;
        static int logged;
        if (getenv("IKARUGA_IBLOG") && logged++ < 20) KLOG("SCRATCH%u = %08X (UMSK=%08X ADDR=%08X)", n, value, s_regs[REG_SCRATCH_UMSK], s_regs[REG_SCRATCH_ADDR]);
        if ((1u << n) & s_regs[REG_SCRATCH_UMSK])
            Wr32(Phys(s_regs[REG_SCRATCH_ADDR] + n * 4), value);
    }
    else if (index == REG_COHER_STATUS_HOST)
        s_regs[index] |= 0x80000000;
}

static bool Compare(uint32_t func, uint32_t value, uint32_t ref)
{
    switch (func & 7)
    {
    case 1: return value < ref;
    case 2: return value <= ref;
    case 3: return value == ref;
    case 4: return value != ref;
    case 5: return value >= ref;
    case 6: return value > ref;
    case 7: return true;
    default: return false;
    }
}

// Lecteur de flux de commandes (anneau circulaire ou tampon indirect linéaire)
struct Reader
{
    uint32_t base, sizeDwords, pos;
    bool ring;
    uint32_t Read() { uint32_t v = Rd32(Phys(base + pos * 4)); pos = ring ? (pos + 1) % sizeDwords : pos + 1; return v; }
    void Skip(uint32_t n) { pos = ring ? (pos + n) % sizeDwords : pos + n; }
};

static void ExecuteIndirect(uint32_t addr, uint32_t sizeDwords);
static uint64_t s_binMask = 0xFFFFFFFFull, s_binSelect = 0xFFFFFFFFull;   // prédication des paquets

// Statistiques (IKARUGA_GPUSTATS=1) pour dimensionner le moteur de rendu
static struct
{
    uint32_t draws = 0, indexed = 0;
    std::map<uint32_t, uint32_t> prims;          // type de primitive -> nombre
    std::set<uint64_t> shaders[2];               // hachages vertex / pixel
    std::set<uint32_t> colorFormats, texFormats;
    std::map<uint32_t, uint32_t> opcodes;
} s_stats;

static uint64_t HashGuest(uint32_t addr, uint32_t bytes)
{
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < bytes; i++) h = (h ^ g_base[addr + i]) * 1099511628211ull;
    return h;
}

static void RecordDraw(uint32_t initiator)
{
    s_stats.draws++;
    s_stats.prims[initiator & 0x3F]++;
    if (((initiator >> 6) & 3) == 0) s_stats.indexed++;
    s_stats.colorFormats.insert((s_regs[0x2001] >> 16) & 0xF);   // RB_COLOR_INFO.format
    for (uint32_t t = 0; t < 32; t++)
    {
        uint32_t d0 = s_regs[0x4800 + t * 6];
        if ((d0 & 3) == 2) s_stats.texFormats.insert(s_regs[0x4800 + t * 6 + 1] & 0x3F);   // fetch texture : format
    }
}

void GpuPrintStats()
{
    static bool on = getenv("IKARUGA_GPUSTATS");
    if (!on) return;
    std::string prims, cf, tf, ops;
    for (auto& [k, v] : s_stats.prims) prims += std::to_string(k) + ":" + std::to_string(v) + " ";
    for (auto f : s_stats.colorFormats) cf += std::to_string(f) + " ";
    for (auto f : s_stats.texFormats) tf += std::to_string(f) + " ";
    for (auto& [k, v] : s_stats.opcodes) { char b[32]; snprintf(b, sizeof b, "%02X:%u ", k, v); ops += b; }
    KLOG("GPU stats : %u dessins (%u indexés), primitives [%s], shaders VS=%zu PS=%zu, formats RT [%s], textures [%s]",
        s_stats.draws, s_stats.indexed, prims.c_str(), s_stats.shaders[0].size(), s_stats.shaders[1].size(), cf.c_str(), tf.c_str());
    KLOG("GPU opcodes : %s", ops.c_str());
    s_stats.draws = s_stats.indexed = 0; s_stats.prims.clear(); s_stats.opcodes.clear();
}

static void ExecutePacket(Reader& r)
{
    uint32_t packet = r.Read();
    switch (packet >> 30)
    {
    case 0:
    {
        uint32_t count = ((packet >> 16) & 0x3FFF) + 1, index = packet & 0x7FFF;
        bool one = (packet >> 15) & 1;
        for (uint32_t i = 0; i < count; i++) WriteRegister(one ? index : index + i, r.Read());
        return;
    }
    case 1:
    {
        uint32_t a = r.Read(), b = r.Read();
        WriteRegister(packet & 0x7FF, a); WriteRegister((packet >> 11) & 0x7FF, b);
        return;
    }
    case 2:
        return;
    }

    uint32_t opcode = (packet >> 8) & 0x7F, count = ((packet >> 16) & 0x3FFF) + 1;
    // Prédication (bit 0 de l'en-tête) : le paquet ne s'exécute que si BIN_SELECT & BIN_MASK != 0 (Xenia)
    if ((packet & 1) && (!(s_binSelect & s_binMask) || opcode == 0x64)) { r.Skip(count); return; }
    s_stats.opcodes[opcode]++;
    s_curOpcode = opcode;
    uint32_t end = r.pos;
    r.Skip(count); uint32_t next = r.pos; r.pos = end;   // position du paquet suivant

    switch (opcode)
    {
    case 0x3F: case 0x37:   // INDIRECT_BUFFER(_PFD)
    {
        uint32_t ptr = r.Read(), len = r.Read() & 0xFFFFF;
        static bool ibLog = getenv("IKARUGA_IBLOG");
        if (ibLog) KLOG("IB %08X %u mots (ring pos %u)", ptr, len, r.ring ? r.pos : 0xFFFFFFFF);
        if (ibLog && len < 40)
        {
            std::string hex;
            for (uint32_t i = 0; i < len; i++) { char b[12]; snprintf(b, sizeof b, "%08X ", Rd32(Phys(ptr + i * 4))); hex += b; }
            KLOG("   %s", hex.c_str());
        }
        ExecuteIndirect(ptr, len);
        break;
    }
    case 0x3C:   // WAIT_REG_MEM : on attend vraiment (le CPU écrit la valeur attendue)
    {
        uint32_t info = r.Read(), addr = r.Read(), ref = r.Read(), mask = r.Read();
        WaitTimer timer{ 0 };
        static bool timing = getenv("IKARUGA_TIMING");
        struct Stat { uint64_t us = 0, n = 0; uint32_t info, ref, mask; };
        static std::map<uint32_t, Stat> stats;
        static auto lastPrint = std::chrono::steady_clock::now();
        struct Rec { std::map<uint32_t, Stat>& st; uint32_t a, i, rf, m; std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
            ~Rec() { auto& s = st[a]; s.us += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count(); s.n++; s.info = i; s.ref = rf; s.mask = m; } };
        std::optional<Rec> rec;
        if (timing)
        {
            rec.emplace(Rec{ stats, addr, info, ref, mask });
            if (std::chrono::steady_clock::now() - lastPrint > std::chrono::seconds(5))
            {
                lastPrint = std::chrono::steady_clock::now();
                for (auto& [a, s] : stats) KLOG("WAIT_REG_MEM %08X info=%X ref=%08X mask=%08X : %llu fois, %llu ms", a, s.info, s.ref, s.mask, (unsigned long long)s.n, (unsigned long long)s.us / 1000);
                stats.clear();
            }
        }
        for (int spins = 0;; spins++)
        {
            if (!(info & 0x10) && (addr & 0x7FFF) == REG_COHER_STATUS_HOST)
                s_regs[REG_COHER_STATUS_HOST] &= ~0x80000000u;   // "MakeCoherent" : rien à synchroniser chez nous
            uint32_t v = (info & 0x10) ? MemRead(addr) : s_regs[addr & 0x7FFF];
            if (Compare(info, v & mask, ref)) break;
            if (spins > 2000000) { KLOG("GPU : WAIT_REG_MEM bloqué info=%X addr=%08X (%08X & %08X vs %08X)", info, addr, v, mask, ref); break; }
            if (spins < 20000) _mm_pause(); else std::this_thread::yield();
        }
        break;
    }
    case 0x21:   // REG_RMW
    {
        uint32_t info = r.Read(), andMask = r.Read(), orMask = r.Read();
        uint32_t v = s_regs[info & 0x1FFF];
        v &= (info >> 31) ? s_regs[andMask & 0x1FFF] : andMask;
        v |= ((info >> 30) & 1) ? s_regs[orMask & 0x1FFF] : orMask;
        WriteRegister(info & 0x1FFF, v);
        break;
    }
    case 0x3E:   // REG_TO_MEM
    {
        uint32_t reg = r.Read(), addr = r.Read();
        MemWrite(addr, s_regs[reg & 0x7FFF]);
        break;
    }
    case 0x3D:   // MEM_WRITE
    {
        uint32_t addr = r.Read();
        for (uint32_t i = 0; i + 1 < count; i++, addr += 4) MemWrite(addr, r.Read());
        break;
    }
    case 0x45:   // COND_WRITE
    {
        uint32_t info = r.Read(), poll = r.Read(), ref = r.Read(), mask = r.Read(), wr = r.Read(), data = r.Read();
        uint32_t v = (info & 0x10) ? MemRead(poll) : s_regs[poll & 0x7FFF];
        if (Compare(info, v & mask, ref))
        {
            if (info & 0x100) MemWrite(wr, data);
            else WriteRegister(wr, data);
        }
        break;
    }
    case 0x58:   // EVENT_WRITE_SHD : fence
    {
        uint32_t initiator = r.Read(), addr = r.Read(), value = r.Read();
        MemWrite(addr, (initiator >> 31) ? s_swapCounter : value);
        static auto last = std::chrono::steady_clock::now();
        if (getenv("IKARUGA_FENCELOG") && std::chrono::steady_clock::now() - last > std::chrono::milliseconds(500))
        {
            last = std::chrono::steady_clock::now();
            KLOG("EVENT_WRITE_SHD initiateur=%08X adresse=%08X valeur=%08X compteur=%u", initiator, addr, value, s_swapCounter);
        }
        break;
    }
    case 0x5A:   // EVENT_WRITE_EXT : étendue d'écran (valeurs maximales, comme Xenia)
    {
        r.Read(); uint32_t addr = r.Read() & ~3u;
        uint16_t ext[6] = { 0, 8192 >> 3, 0, 8192 >> 3, 0, 1 };
        for (int i = 0; i < 6; i++) Wr16(Phys(addr + i * 2), ext[i]);
        break;
    }
    case 0x54:   // INTERRUPT
    {
        uint32_t mask = r.Read();
        for (uint32_t n = 0; n < 6; n++) if (mask & (1u << n)) GpuDispatchInterrupt(1, n);
        break;
    }
    case 0x22:   // DRAW_INDX : (viz query, initiateur, ...)
    {
        r.Read(); uint32_t initiator = r.Read();
        uint32_t dmaBase = 0, dmaSize = 0;
        if (((initiator >> 6) & 3) == 0) { dmaBase = r.Read(); dmaSize = r.Read(); }   // index en mémoire (VGT_DMA_BASE/SIZE)
        RecordDraw(initiator);
        RenderDraw(s_regs, initiator, Phys, dmaBase, dmaSize);
        break;
    }
    case 0x36:   // DRAW_INDX_2 : (initiateur, données inline)
    {
        uint32_t initiator = r.Read();
        RecordDraw(initiator);
        RenderDraw(s_regs, initiator, Phys);
        break;
    }
    case 0x60: s_binMask = (s_binMask & 0xFFFFFFFF00000000ull) | r.Read(); break;                      // SET_BIN_MASK_LO
    case 0x61: s_binMask = (s_binMask & 0xFFFFFFFFull) | (uint64_t(r.Read()) << 32); break;             // SET_BIN_MASK_HI
    case 0x62: s_binSelect = (s_binSelect & 0xFFFFFFFF00000000ull) | r.Read(); break;                  // SET_BIN_SELECT_LO
    case 0x63: s_binSelect = (s_binSelect & 0xFFFFFFFFull) | (uint64_t(r.Read()) << 32); break;         // SET_BIN_SELECT_HI
    case 0x50: { uint64_t hi = r.Read(); s_binMask = (hi << 32) | r.Read(); break; }                    // SET_BIN_MASK
    case 0x51: { uint64_t hi = r.Read(); s_binSelect = (hi << 32) | r.Read(); break; }                  // SET_BIN_SELECT
    case 0x64:   // présentation (paquet écrit par notre VdSwap, comme PM4_XE_SWAP de Xenia)
        RenderPresent(r.Read());   // adresse du tampon d'affichage écrite par VdSwap
        break;
    case 0x2D:   // SET_CONSTANT : écrit une suite de registres (ALU, fetch, bool, loop, registres)
    {
        uint32_t info = r.Read(), index = info & 0x7FF;
        static const uint32_t bases[] = { 0x4000, 0x4800, 0x4900, 0x4908, 0x2000 };
        uint32_t type = (info >> 16) & 0xFF;
        for (uint32_t i = 0; i + 1 < count; i++) WriteRegister((type < 5 ? bases[type] : 0x2000) + index + i, r.Read());
        break;
    }
    case 0x2F:   // LOAD_ALU_CONSTANT : copie depuis la mémoire
    {
        uint32_t addr = r.Read() & 0x3FFFFFFF, info = r.Read(), size = r.Read() & 0xFFF;
        static const uint32_t bases[] = { 0x4000, 0x4800, 0x4900, 0x4908, 0x2000 };
        uint32_t type = (info >> 16) & 0xFF, index = info & 0x7FF;
        for (uint32_t i = 0; i < size; i++) WriteRegister((type < 5 ? bases[type] : 0x2000) + index + i, Rd32(Phys(addr + i * 4)));
        break;
    }
    case 0x27:   // IM_LOAD : shader en mémoire (adresse | type, début << 16 | taille)
    {
        uint32_t addrType = r.Read(), size = r.Read() & 0xFFFF;
        uint64_t h = HashGuest(Phys(addrType & ~3u), size * 4);
        const TranslatedShader* shader = ShaderLookup(g_base + Phys(addrType & ~3u), size * 4, (addrType & 3) == 1);
        RenderSetShader((addrType & 3) == 1, g_base + Phys(addrType & ~3u), size * 4, shader);
        if (shader && s_stats.shaders[(addrType & 3) == 1].count(h) == 0 && getenv("IKARUGA_GPUSTATS"))
            KLOG("  -> shader traduit %016llx (%zu mots SPIR-V)", (unsigned long long)shader->containerHash, shader->spirv.size());
        if (s_stats.shaders[(addrType & 3) == 1].insert(h).second && getenv("IKARUGA_GPUSTATS"))
        {
            KLOG("IM_LOAD type=%u taille=%u fnv=%016llx", addrType & 3, size * 4, (unsigned long long)h);
            std::string hex;
            for (uint32_t i = 0; i < size; i++) { char b[10]; snprintf(b, sizeof b, "%08x ", Rd32(Phys(addrType & ~3u) + i * 4)); hex += b; }
            KLOG("  ucode %s", hex.c_str());
        }
        break;
    }
    case 0x2B:   // IM_LOAD_IMMEDIATE : shader inline
    {
        uint32_t type = r.Read() & 3, size = r.Read() & 0xFFFF;
        uint32_t pos = r.pos;
        {
            std::vector<uint8_t> ucode(size * 4);
            for (uint32_t i = 0; i < size; i++) { uint32_t v = __builtin_bswap32(r.Read()); memcpy(&ucode[i * 4], &v, 4); }
            r.pos = pos;
            RenderSetShader(type == 1, ucode.data(), size * 4, ShaderLookup(ucode.data(), size * 4, type == 1));
        }
        uint64_t h = 1469598103934665603ull;
        for (uint32_t i = 0; i < size; i++) h = (h ^ r.Read()) * 1099511628211ull;
        s_stats.shaders[type == 1].insert(h);
        r.pos = pos;
        break;
    }
    default:
        break;
    }
    r.pos = next;
}

static void ExecuteIndirect(uint32_t addr, uint32_t sizeDwords)
{
    Reader r{ addr, sizeDwords, 0, false };
    while (r.pos < sizeDwords) ExecutePacket(r);
}

void DebugRegisterThread(const char* name);

// Attente du thread GPU : sous Windows un sleep_for de 100 µs dure 1 à 15 ms, et le CPU attend le GPU à chaque
// image ; on tourne d'abord activement (~2 ms) avant de céder la main.
static void GpuIdle(uint32_t& idleSpins)
{
    if (++idleSpins < 20000) { _mm_pause(); return; }
    if (idleSpins < 40000) { std::this_thread::yield(); return; }
    std::this_thread::sleep_for(std::chrono::microseconds(100));
}

static void GpuThread()
{
    DebugRegisterThread("gpu");
    GuestInitThreadContext(s_ctx, 0x10000);
    uint32_t idleSpins = 0;
    for (;;)
    {
        uint32_t wptr = Rd32(MMIO_BASE + REG_CP_RB_WPTR * 4);
        if (!s_ringBase || wptr == s_rptr) { GpuIdle(idleSpins); continue; }
        idleSpins = 0;
        // N'exécute que des paquets entièrement publiés : un paquet qui dépasse wptr attend la suite
        // (sinon la lecture saute par-dessus wptr et refait tout le tour du ring)
        uint32_t size = s_ringSize / 4;
        Reader r{ s_ringBase, size, s_rptr, true };
        while (r.pos != wptr)
        {
            uint32_t available = (wptr + size - r.pos) % size;
            // Un wptr qui recule (vu en jeu : -1) donnerait "presque un tour de ring" : ce n'est pas du
            // travail nouveau, on ne rejoue pas d'anciennes commandes
            if (available > size - 256)
            {
                static int warned;
                if (warned++ < 5) KLOG("GPU : wptr %X derrière rptr %X, ignoré", wptr, r.pos);
                break;
            }
            uint32_t header = Rd32(Phys(s_ringBase + r.pos * 4));
            uint32_t length;
            switch (header >> 30)
            {
            case 0: length = 1 + ((header >> 16) & 0x3FFF) + 1; break;
            case 1: length = 3; break;
            case 2: length = 1; break;
            default: length = 1 + ((header >> 16) & 0x3FFF) + 1; break;
            }
            if (length > available) break;
            ExecutePacket(r);
        }
        s_rptr = r.pos;
        g_gpuRptr = s_rptr; g_gpuLastWptr = wptr;
        if (s_rptrWriteBack) Wr32(s_rptrWriteBack, s_rptr);
        if (r.pos != wptr) GpuIdle(idleSpins);
    }
}

// Registres MMIO que le jeu lit directement en mémoire : valeurs fixes reprises de Xenia (ReadRegister)
static void InitMmio()
{
    static std::once_flag once;
    std::call_once(once, [] {
        Wr32(MMIO_BASE + 0x0F00 * 4, 0x08100748);   // RB_EDRAM_TIMING
        Wr32(MMIO_BASE + 0x0F01 * 4, 0x0000200E);   // RB_BC_CONTROL
        Wr32(MMIO_BASE + 0x194C * 4, 0x000002D0);   // D1MODE_V_COUNTER
        Wr32(MMIO_BASE + 0x1951 * 4, 1);            // statut d'interruption : vblank
        Wr32(MMIO_BASE + 0x1961 * 4, 0x050002D0);   // D1MODE_VIEWPORT_SIZE 1280x720
    });
}

void GpuInitializeRingBuffer(uint32_t base, uint32_t sizeLog2)
{
    InitMmio();
    s_ringBase = base;
    s_ringSize = 1u << (sizeLog2 + 3);
    s_rptr = 0;
    static std::once_flag once;
    std::call_once(once, [] { std::thread(GpuThread).detach(); });
}

void GpuEnableReadPointerWriteBack(uint32_t addr) { s_rptrWriteBack = Phys(addr); }
void GpuSetInterruptCallback(uint32_t cb, uint32_t ctx) { InitMmio(); s_interruptCallback = cb; s_interruptCallbackCtx = ctx; }
void GpuOnSwap() { s_swapCounter++; }
