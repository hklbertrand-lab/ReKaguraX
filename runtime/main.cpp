// Ikaruga recompilé : point d'entrée, mémoire invitée, chargement du XEX, fenêtre SDL3
#include "guest.h"
#include "vulkan.h"
#include "shaders.h"
#include <image.h>
#include <xex.h>
#include <string>
#include <unordered_map>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <thread>
#include <chrono>
#include <vector>
#include <filesystem>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

uint8_t* g_base;
std::filesystem::path g_gameDir = "extracted";
SDL_Window* g_window;

static std::mutex s_allocMutex;
static uint32_t s_kernelTop = 0x70000000, s_virtualTop = 0x40000000, s_physicalTop = 0xA0000000;

static uint32_t Bump(uint32_t& top, uint32_t size, uint32_t align, uint32_t limit)
{
    std::lock_guard lock(s_allocMutex);
    uint32_t addr = (top + align - 1) & ~(align - 1);
    if (uint64_t(addr) + size > limit) { KLOG("plus de mémoire invitée (%08X + %X)", addr, size); abort(); }
    top = addr + size;
    memset(g_base + addr, 0, size);
    return addr;
}

uint32_t GuestAllocKernel(uint32_t size, uint32_t align) { return Bump(s_kernelTop, size, align, 0x7FFF0000); }
uint32_t GuestAllocVirtual(uint32_t size) { return Bump(s_virtualTop, (size + 0xFFFF) & ~0xFFFF, 0x10000, 0x70000000); }
uint32_t GuestAllocPhysical(uint32_t size, uint32_t align) { return Bump(s_physicalTop, size, align < 0x1000 ? 0x1000 : align, 0xC0000000); }

void GuestInitThreadContext(PPCContext& ctx, uint32_t stackSize)
{
    memset(&ctx, 0, sizeof(ctx));
    stackSize = (stackSize ? stackSize : 0x40000) + 0x1000;
    uint32_t stack = GuestAllocKernel(stackSize, 0x1000);
    // PCR alignés sur 64 Ko : le code fait "mtmsrd r13" pour masquer les interruptions, le bit EE (0x8000) doit y valoir 0
    uint32_t pcr = GuestAllocKernel(0x2D8 + 0x400, 0x10000);   // KPCR + KTHREAD + slots TLS
    uint32_t thread = pcr + 0x2D8;
    uint32_t tls = thread + 0x200;
    static std::atomic<uint32_t> nextId = 1;

    Wr32(pcr + 0x00, tls);              // TlsData
    Wr32(pcr + 0x70, stack + stackSize); // StackBase
    Wr32(pcr + 0x74, stack);            // StackLimit
    Wr32(pcr + 0x100, thread);          // CurrentThread
    Wr8 (pcr + 0x10C, 0);               // CurrentProcessor
    Wr32(thread + 0x14C, nextId++);     // ThreadId

    ctx.r1.u64 = stack + stackSize - 0x100;
    ctx.r13.u64 = pcr;
    ctx.fpscr.loadFromHost();
    ctx.msr = 0x8000;   // interruptions autorisées (EE)
}

// Mémoire invitée (4 Go). La mémoire physique (512 Mo) est visible à 0xA0000000 et 0xC0000000 :
// ces deux plages sont deux vues d'une même section (le jeu et D3D passent de l'une à l'autre).
// La vue 0xE0000000 (décalée de 4 Ko chez Xenia) reste indépendante pour l'instant.
static constexpr uint64_t kPhysicalSize = 0x20000000;

static bool ReserveGuestMemory()
{
#ifdef _WIN32
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, DWORD(kPhysicalSize), nullptr);
    if (!section) return false;
    for (int attempt = 0; attempt < 8 && !g_base; attempt++)
    {
        // Trouve 4 Go libres, les libère, puis y place les morceaux à adresses fixes
        uint8_t* p = (uint8_t*)VirtualAlloc(nullptr, PPC_MEMORY_SIZE, MEM_RESERVE, PAGE_NOACCESS);
        if (!p) return false;
        VirtualFree(p, 0, MEM_RELEASE);
        bool ok = VirtualAlloc(p, 0xA0000000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) &&
            MapViewOfFileEx(section, FILE_MAP_ALL_ACCESS, 0, 0, kPhysicalSize, p + 0xA0000000) &&
            MapViewOfFileEx(section, FILE_MAP_ALL_ACCESS, 0, 0, kPhysicalSize, p + 0xC0000000) &&
            VirtualAlloc(p + 0xE0000000, 0x20000000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (ok) g_base = p;
        else
        {
            VirtualFree(p, 0, MEM_RELEASE);
            UnmapViewOfFile(p + 0xA0000000);
            UnmapViewOfFile(p + 0xC0000000);
            VirtualFree(p + 0xE0000000, 0, MEM_RELEASE);
        }
    }
#else
    int fd = memfd_create("ikaruga-physique", 0);
    if (fd < 0 || ftruncate(fd, kPhysicalSize) != 0) return false;
    g_base = (uint8_t*)mmap(nullptr, PPC_MEMORY_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (g_base == MAP_FAILED) return g_base = nullptr, false;
    for (uint64_t view : { 0xA0000000ull, 0xC0000000ull })
        if (mmap(g_base + view, kPhysicalSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED) return false;
#endif
    return g_base != nullptr;
}

extern std::unordered_map<size_t, const char*> XboxKernelExports;
uint32_t KernelDataExport(const char* name);   // kernel.cpp

// XenonUtils remplace les imports de fonctions par des thunks, mais laisse les imports de
// variables (type 0) : chaque slot doit pointer vers une donnée noyau allouée par nous.
static void PatchDataImports(const uint8_t* xex)
{
    auto* hdr = (const Xex2ImportHeader*)getOptHeaderPtr(xex, XEX_HEADER_IMPORT_LIBRARIES);
    if (!hdr) return;
    const char* strings = (const char*)(hdr + 1);
    std::vector<std::string> libs;
    for (uint32_t i = 0, off = 0; i < hdr->numImports; i++)
    {
        libs.emplace_back(strings + off);
        off += strlen(strings + off) + 1;
        while (off < hdr->sizeOfStringTable && strings[off] == 0) off++;
    }
    auto* lib = (const Xex2ImportLibrary*)((const char*)(hdr + 1) + hdr->sizeOfStringTable);
    for (uint32_t l = 0; l < hdr->numImports; l++)
    {
        auto* desc = (const Xex2ImportDescriptor*)(lib + 1);
        bool kernel = libs[lib->name] == "xboxkrnl.exe";
        for (uint32_t i = 0; i < lib->numberOfImports; i++)
        {
            // Une fonction importée a deux enregistrements : son slot d'IAT puis son thunk,
            // que XenonUtils a remplacé par des nops (0x60000000). Une variable n'en a qu'un.
            auto isThunk = [&](uint32_t n) { return n < lib->numberOfImports && Rd32(desc[n].firstThunk) == 0x60000000; };
            uint32_t slot = desc[i].firstThunk;
            uint32_t value = *(uint32_t*)(g_base + slot);   // XenonUtils l'a laissé en ordre natif
            if (isThunk(i) || isThunk(i + 1) || (value >> 24) != 0) continue;
            uint32_t ordinal = value & 0xFFFF;
            const char* name = "?";
            if (kernel)
                if (auto it = XboxKernelExports.find(ordinal); it != XboxKernelExports.end()) name = it->second;
            uint32_t addr = KernelDataExport(name);
            Wr32(slot, addr);
            KLOG("import de donnée %s!%s (ordinal %u) -> %08X", libs[lib->name].c_str(), name, ordinal, addr);
        }
        lib = (const Xex2ImportLibrary*)((const char*)(lib + 1) + lib->numberOfImports * sizeof(Xex2ImportDescriptor));
    }
}

static bool LoadExecutable()
{
    FILE* f = fopen((g_gameDir / "default.xex").string().c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> data;
    fseek(f, 0, SEEK_END); data.resize(ftell(f)); fseek(f, 0, SEEK_SET);
    fread(data.data(), 1, data.size(), f); fclose(f);

    Image img = Image::ParseImage(data.data(), data.size());
    memcpy(g_base + img.base, img.data.get(), img.size);
    PatchDataImports(data.data());
    ShadersInit(PPC_IMAGE_BASE, PPC_IMAGE_SIZE);

    // Table de pointeurs de fonctions juste après l'image, comme l'attend PPC_LOOKUP_FUNC
    for (PPCFuncMapping* m = PPCFuncMappings; m->host; m++)
        *(PPCFunc**)(g_base + PPC_IMAGE_BASE + PPC_IMAGE_SIZE + uint64_t(m->guest - PPC_CODE_BASE) * 2) = m->host;
    return true;
}

#ifdef _WIN32
static std::atomic<HANDLE> s_gameThread;
static std::mutex s_debugThreadsMutex;
static std::vector<std::pair<std::string, HANDLE>> s_debugThreads;
#endif

// Débogage : rend un thread visible pour IKARUGA_SAMPLE
void DebugRegisterThread(const char* name)
{
#ifdef _WIN32
    std::lock_guard l(s_debugThreadsMutex);
    s_debugThreads.push_back({ name, OpenThread(THREAD_ALL_ACCESS, FALSE, GetCurrentThreadId()) });
#endif
}

static void GuestMain()
{
#ifdef _WIN32
    s_gameThread = OpenThread(THREAD_ALL_ACCESS, FALSE, GetCurrentThreadId());
#endif
    static PPCContext ctx;
    GuestInitThreadContext(ctx, 0x100000);
    KLOG("démarrage du jeu à %08X", 0x8217BA20);
    GuestCall(ctx, 0x8217BA20);
    KLOG("le jeu est sorti de son point d'entrée");
}

#ifdef _WIN32
// Débogage : IKARUGA_WATCH=adresse invitée -> journalise chaque écriture du code hôte dans cette page
static uint32_t s_watch;
static void* WatchPage() { return g_base + (s_watch & ~0xFFFu); }

static LONG CALLBACK CrashHandler(EXCEPTION_POINTERS* e)
{
    auto* r = e->ExceptionRecord;
    if (s_watch && r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->ExceptionInformation[0] == 1 &&
        (uint8_t*)r->ExceptionInformation[1] - (uint8_t*)WatchPage() < 0x1000)
    {
        uint32_t guest = uint32_t((uint8_t*)r->ExceptionInformation[1] - g_base);
        if ((guest & ~0x3Fu) == (s_watch & ~0x3Fu))
            fprintf(stderr, "[watch] écriture %08X depuis exe+%llX\n", guest,
                (unsigned long long)((uint8_t*)r->ExceptionAddress - (uint8_t*)GetModuleHandleW(nullptr)));
        if ((guest & ~0x3Fu) == (s_watch & ~0x3Fu))
        {
            void* frames[12]; USHORT n = CaptureStackBackTrace(0, 12, frames, nullptr);
            for (USHORT i = 0; i < n; i++)
                fprintf(stderr, "[watch]   pile exe+%llX\n",(unsigned long long)((uint8_t*)frames[i] - (uint8_t*)GetModuleHandleW(nullptr)));
        }
        DWORD old; VirtualProtect(WatchPage(), 0x1000, PAGE_READWRITE, &old);
        e->ContextRecord->EFlags |= 0x100;   // pas à pas pour reprotéger après l'écriture
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (s_watch && r->ExceptionCode == EXCEPTION_SINGLE_STEP)
    {
        DWORD old; VirtualProtect(WatchPage(), 0x1000, PAGE_READONLY, &old);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (r->ExceptionCode < 0x80000000) return EXCEPTION_CONTINUE_SEARCH;
    uint64_t addr = r->NumberParameters >= 2 ? r->ExceptionInformation[1] : 0;
    fprintf(stderr, "[crash] exception %08lX à exe+%llX, adresse %llX (invitée %08llX)\n", r->ExceptionCode,
        (unsigned long long)((uint8_t*)r->ExceptionAddress - (uint8_t*)GetModuleHandleW(nullptr)), (unsigned long long)addr, (unsigned long long)(addr - (uint64_t)g_base));
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

int main(int argc, char** argv)
{
#ifdef _WIN32
    AddVectoredExceptionHandler(1, CrashHandler);
#endif
    if (argc > 1) g_gameDir = argv[1];
    if (!ReserveGuestMemory()) { fprintf(stderr, "Impossible de réserver 4 Go de mémoire invitée\n"); return 1; }
    if (!LoadExecutable()) { fprintf(stderr, "default.xex introuvable dans %s\n", g_gameDir.string().c_str()); return 1; }

    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO);
    g_window = SDL_CreateWindow("Ikaruga", 1280, 720, SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN);
    if (!VulkanInit(g_window)) fprintf(stderr, "Vulkan indisponible : pas d'affichage\n");

#ifdef _WIN32
    if (const char* w = getenv("IKARUGA_WATCH"))
    {
        s_watch = strtoul(w, nullptr, 16);
        DWORD old; VirtualProtect(WatchPage(), 0x1000, PAGE_READONLY, &old);
    }
#endif
    std::thread game(GuestMain);
#ifdef _WIN32
    // Débogage : IKARUGA_SAMPLE=1 -> affiche toutes les 2 s où se trouve le thread principal du jeu
    if (getenv("IKARUGA_SAMPLE"))
    {
        std::thread([] {
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                std::vector<std::pair<std::string, HANDLE>> threads{ { "principal", s_gameThread.load() } };
                { std::lock_guard l(s_debugThreadsMutex); threads.insert(threads.end(), s_debugThreads.begin(), s_debugThreads.end()); }
                for (auto& [name, h] : threads)
                {
                    SuspendThread(h);
                    CONTEXT c{}; c.ContextFlags = CONTEXT_CONTROL;
                    GetThreadContext(h, &c);
                    fprintf(stderr, "[sample] %s rip exe+%llX\n", name.c_str(), (unsigned long long)(c.Rip - (uint64_t)GetModuleHandleW(nullptr)));
                    ResumeThread(h);
                }
            }
        }).detach();
    }
#endif
    game.detach();

    for (bool running = true; running;)
    {
        SDL_Event e;
        while (SDL_WaitEventTimeout(&e, 16))
            if (e.type == SDL_EVENT_QUIT) running = false;
    }
    SDL_Quit();
    return 0;
}
