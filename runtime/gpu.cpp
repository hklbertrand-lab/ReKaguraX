// Processeur de commandes GPU Xenos (PM4) minimal : registres, écritures mémoire, fences, interruptions.
// Pas encore de rendu : les dessins sont ignorés. Sémantique des paquets reprise de Xenia (command_processor.cc).
#include "guest.h"
#include <chrono>
#include <thread>

static uint32_t s_regs[0x8000];
static uint32_t s_ringBase, s_ringSize, s_rptr, s_rptrWriteBack;
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
static void MemWrite(uint32_t addr, uint32_t value) { Wr32(Phys(addr & ~3u), __builtin_bswap32(GpuSwap(value, addr & 3))); }
static uint32_t MemRead(uint32_t addr) { return GpuSwap(__builtin_bswap32(Rd32(Phys(addr & ~3u))), addr & 3); }

void GpuDispatchInterrupt(uint32_t source, uint32_t cpu)
{
    if (!s_interruptCallback) return;
    s_ctx.r3.u64 = source;
    s_ctx.r4.u64 = s_interruptCallbackCtx;
    GuestCall(s_ctx, s_interruptCallback);
}

static void WriteRegister(uint32_t index, uint32_t value)
{
    if (index >= 0x8000) return;
    s_regs[index] = value;
    if (index >= REG_SCRATCH_REG0 && index < REG_SCRATCH_REG0 + 8)
    {
        uint32_t n = index - REG_SCRATCH_REG0;
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
    uint32_t end = r.pos;
    r.Skip(count); uint32_t next = r.pos; r.pos = end;   // position du paquet suivant

    switch (opcode)
    {
    case 0x3F: case 0x37:   // INDIRECT_BUFFER(_PFD)
    {
        uint32_t ptr = r.Read(), len = r.Read() & 0xFFFFF;
        ExecuteIndirect(ptr, len);
        break;
    }
    case 0x3C:   // WAIT_REG_MEM : on attend vraiment (le CPU écrit la valeur attendue)
    {
        uint32_t info = r.Read(), addr = r.Read(), ref = r.Read(), mask = r.Read();
        for (int spins = 0;; spins++)
        {
            if (!(info & 0x10) && (addr & 0x7FFF) == REG_COHER_STATUS_HOST)
                s_regs[REG_COHER_STATUS_HOST] &= ~0x80000000u;   // "MakeCoherent" : rien à synchroniser chez nous
            uint32_t v = (info & 0x10) ? MemRead(addr) : s_regs[addr & 0x7FFF];
            if (Compare(info, v & mask, ref)) break;
            if (spins > 2000000) { KLOG("GPU : WAIT_REG_MEM bloqué (%08X & %08X vs %08X)", v, mask, ref); break; }
            std::this_thread::yield();
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
    case 0x22: case 0x36:   // DRAW_INDX(_2) : pas encore de rendu
        break;
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

static void GpuThread()
{
    GuestInitThreadContext(s_ctx, 0x10000);
    for (;;)
    {
        uint32_t wptr = Rd32(MMIO_BASE + REG_CP_RB_WPTR * 4);
        if (!s_ringBase || wptr == s_rptr) { std::this_thread::sleep_for(std::chrono::microseconds(100)); continue; }
        Reader r{ s_ringBase, s_ringSize / 4, s_rptr, true };
        while (r.pos != wptr) ExecutePacket(r);
        s_rptr = wptr;
        if (s_rptrWriteBack) Wr32(s_rptrWriteBack, s_rptr);
    }
}

void GpuInitializeRingBuffer(uint32_t base, uint32_t sizeLog2)
{
    s_ringBase = base;
    s_ringSize = 1u << (sizeLog2 + 3);
    s_rptr = 0;
    static std::once_flag once;
    std::call_once(once, [] { std::thread(GpuThread).detach(); });
}

void GpuEnableReadPointerWriteBack(uint32_t addr) { s_rptrWriteBack = Phys(addr); }
void GpuSetInterruptCallback(uint32_t cb, uint32_t ctx) { s_interruptCallback = cb; s_interruptCallbackCtx = ctx; }
void GpuOnSwap() { s_swapCounter++; }
