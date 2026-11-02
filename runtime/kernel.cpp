// Réimplémentation des fonctions noyau Xbox 360 (xboxkrnl / xam) utilisées par Ikaruga
#include "guest.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#define KFUNC(name) PPC_FUNC(__imp__##name)
#define ARG(n) ctx.r##n.u32
#define RET(v) do { ctx.r3.u64 = (uint32_t)(v); return; } while (0)

enum : uint32_t
{
    STATUS_SUCCESS = 0, STATUS_TIMEOUT = 0x102, STATUS_PENDING = 0x103,
    STATUS_UNSUCCESSFUL = 0xC0000001, STATUS_INVALID_HANDLE = 0xC0000008,
    STATUS_END_OF_FILE = 0xC0000011, STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034,
    STATUS_NO_MORE_FILES = 0x80000006,
};

extern std::filesystem::path g_gameDir;

// ---------------------------------------------------------------- objets noyau

struct KObject
{
    virtual ~KObject() = default;
    virtual bool Wait(int64_t timeoutMs) { return true; }   // false = délai dépassé
    virtual void Signal() {}
};

struct KEvent : KObject
{
    std::mutex m; std::condition_variable cv;
    bool signaled = false, manualReset = false;
    bool Wait(int64_t timeoutMs) override
    {
        std::unique_lock lock(m);
        auto ready = [&] { return signaled; };
        if (timeoutMs < 0) cv.wait(lock, ready);
        else if (!cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), ready)) return false;
        if (!manualReset) signaled = false;
        return true;
    }
    void Signal() override { { std::lock_guard l(m); signaled = true; } manualReset ? cv.notify_all() : cv.notify_one(); }
    void Reset() { std::lock_guard l(m); signaled = false; }
};

struct KSemaphore : KObject
{
    std::mutex m; std::condition_variable cv;
    int32_t count = 0, max = 1;
    bool Wait(int64_t timeoutMs) override
    {
        std::unique_lock lock(m);
        auto ready = [&] { return count > 0; };
        if (timeoutMs < 0) cv.wait(lock, ready);
        else if (!cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), ready)) return false;
        count--;
        return true;
    }
    int32_t Release(int32_t n) { std::lock_guard l(m); int32_t prev = count; count = std::min(count + n, max); cv.notify_all(); return prev; }
};

struct KThread : KEvent   // signalé quand le thread se termine
{
    uint32_t guestThread = 0, startAddr = 0, startCtx = 0, xapiStartup = 0, stackSize = 0;
    std::mutex suspendMutex; std::condition_variable suspendCv; bool suspended = false;
    PPCContext ctx;   // préparé à la création : le KTHREAD invité existe même si le thread est suspendu
    KThread() { manualReset = true; }
};

struct KFile : KObject
{
    FILE* f = nullptr;
    std::filesystem::path path;
    bool isDir = false;
    ~KFile() override { if (f) fclose(f); }
};

static std::mutex s_objMutex;
static std::unordered_map<uint32_t, std::shared_ptr<KObject>> s_handles;     // handle -> objet
static std::unordered_map<uint32_t, std::shared_ptr<KObject>> s_dispatchers; // adresse KEVENT invitée -> objet
static uint32_t s_nextHandle = 0xF8000004;

static uint32_t NewHandle(std::shared_ptr<KObject> obj)
{
    std::lock_guard l(s_objMutex);
    uint32_t h = s_nextHandle; s_nextHandle += 4;
    s_handles[h] = std::move(obj);
    return h;
}

static std::shared_ptr<KObject> FromHandle(uint32_t h)
{
    std::lock_guard l(s_objMutex);
    auto it = s_handles.find(h);
    return it == s_handles.end() ? nullptr : it->second;
}

// Objet hôte associé à une structure DISPATCHER_HEADER invitée (créé à la première utilisation)
// Objet "dispatcher" invité (KEVENT, KSEMAPHORE...) : l'état vit dans la structure DISPATCHER_HEADER du jeu
// (type à +0, état de signal à +4, limite de sémaphore à +0x10), car les jeux l'initialisent et le
// modifient aussi directement en mémoire. Les attentes interrogent cet état (réveil par condition + 2 ms).
static std::mutex s_dispatchMutex;
static std::condition_variable s_dispatchCv;

struct KGuestDispatcher : KObject
{
    uint32_t addr;
    explicit KGuestDispatcher(uint32_t a) : addr(a) {}

    bool TryAcquire()
    {
        uint8_t type = Rd8(addr);
        int32_t state = int32_t(Rd32(addr + 4));
        if (state <= 0) return false;
        if (type == 1) Wr32(addr + 4, 0);                   // événement de synchronisation : auto-reset
        else if (type == 5) Wr32(addr + 4, state - 1);      // sémaphore
        return true;
    }
    bool Wait(int64_t timeoutMs) override
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs < 0 ? 0 : timeoutMs);
        std::unique_lock lock(s_dispatchMutex);
        for (;;)
        {
            if (TryAcquire()) return true;
            if (timeoutMs >= 0 && std::chrono::steady_clock::now() >= deadline) return false;
            s_dispatchCv.wait_for(lock, std::chrono::milliseconds(2));
        }
    }
    void Signal() override
    {
        { std::lock_guard lock(s_dispatchMutex); Wr32(addr + 4, 1); }
        s_dispatchCv.notify_all();
    }
    void Reset() { std::lock_guard lock(s_dispatchMutex); Wr32(addr + 4, 0); }
    int32_t Release(int32_t n)
    {
        int32_t prev;
        {
            std::lock_guard lock(s_dispatchMutex);
            prev = int32_t(Rd32(addr + 4));
            int32_t limit = int32_t(Rd32(addr + 0x10));
            Wr32(addr + 4, uint32_t(limit > 0 ? std::min(prev + n, limit) : prev + n));
        }
        s_dispatchCv.notify_all();
        return prev;
    }
};

static std::unordered_map<uint32_t, std::shared_ptr<KObject>> s_threadDispatchers;   // KTHREAD invité -> thread hôte

static std::shared_ptr<KObject> FromDispatcher(uint32_t addr)
{
    std::lock_guard l(s_objMutex);
    if (auto it = s_threadDispatchers.find(addr); it != s_threadDispatchers.end()) return it->second;
    if (auto it = s_dispatchers.find(addr); it != s_dispatchers.end()) return it->second;
    uint8_t type = Rd8(addr);
    if (type != 0 && type != 1 && type != 5) KLOG("objet dispatcher de type %u à %08X traité comme un événement", type, addr);
    auto obj = std::make_shared<KGuestDispatcher>(addr);
    s_dispatchers[addr] = obj;
    return obj;
}

// Délai Xbox (100 ns, négatif = relatif) -> millisecondes, -1 = infini
static int64_t TimeoutMs(uint32_t pTimeout)
{
    if (!pTimeout) return -1;
    int64_t t = (int64_t)Rd64(pTimeout);
    return t < 0 ? -t / 10000 : 0;
}

// ---------------------------------------------------------------- données exportées

uint32_t KernelDataExport(const char* name)
{
    if (!strncmp(name, "__imp__", 7)) name += 7;
    uint32_t addr = GuestAllocKernel(0x400);
    if (!strcmp(name, "XboxHardwareInfo")) { Wr32(addr, 0x20); Wr8(addr + 4, 6); }   // 6 threads matériels
    else if (!strcmp(name, "KeTimeStampBundle")) Wr32(addr + 0x10, 50000000);          // tick 50 MHz
    else if (!strcmp(name, "XexExecutableModuleHandle")) Wr32(addr, addr + 0x100);
    else if (!strcmp(name, "VdGpuClockInMHz")) Wr32(addr, 500);
    else if (!strcmp(name, "XboxKrnlVersion")) { Wr16(addr, 2); Wr16(addr + 2, 0); Wr16(addr + 4, 17559); Wr16(addr + 6, 0); }
    else if (!strcmp(name, "ExLoadedImageName")) strcpy((char*)g_base + addr, "\\Device\\Harddisk0\\default.xex");
    return addr;
}

// ---------------------------------------------------------------- journal des imports non implémentés

void LogImport(const char* name, PPCContext& ctx)
{
    static std::mutex m;
    static std::unordered_map<std::string, int> seen;
    std::lock_guard l(m);
    if (seen[name]++ < 3)
        KLOG("non implémenté : %s(%08X, %08X, %08X, %08X)", name, ARG(3), ARG(4), ARG(5), ARG(6));
}

// ---------------------------------------------------------------- mémoire

KFUNC(MmAllocatePhysicalMemoryEx)   // (flags, size, protect, minAddr, maxAddr, alignment)
{
    RET(GuestAllocPhysical(ARG(4), ARG(8) ? ARG(8) : 0x1000));
}
KFUNC(MmFreePhysicalMemory) { RET(0); }
KFUNC(MmGetPhysicalAddress) { RET(ARG(3) & 0x1FFFFFFF); }
KFUNC(MmQueryAddressProtect) { RET(4); }   // PAGE_READWRITE
KFUNC(MmQueryAllocationSize) { RET(0x10000); }

KFUNC(NtAllocateVirtualMemory)   // (pBase, pSize, allocType, protect)
{
    uint32_t size = Rd32(ARG(4));
    if (Rd32(ARG(3)) == 0) Wr32(ARG(3), GuestAllocVirtual(size));
    Wr32(ARG(4), (size + 0xFFFF) & ~0xFFFF);
    RET(STATUS_SUCCESS);
}
KFUNC(NtFreeVirtualMemory) { RET(STATUS_SUCCESS); }

KFUNC(ExAllocatePoolTypeWithTag) { RET(GuestAllocVirtual(ARG(3))); }
KFUNC(ExAllocatePoolWithTag) { RET(GuestAllocVirtual(ARG(3))); }
KFUNC(ExFreePool) { RET(0); }
KFUNC(XamAlloc) { Wr32(ARG(5), GuestAllocVirtual(ARG(4))); RET(STATUS_SUCCESS); }   // (flags, size, pOut)
KFUNC(XamFree) { RET(0); }

KFUNC(RtlFillMemoryUlong) { for (uint32_t i = 0; i < ARG(4) / 4; i++) Wr32(ARG(3) + i * 4, ARG(5)); }
KFUNC(RtlCompareMemoryUlong)
{
    uint32_t n = 0;
    while (n + 4 <= ARG(4) && Rd32(ARG(3) + n) == ARG(5)) n += 4;
    RET(n);
}

// ---------------------------------------------------------------- sections critiques

static std::mutex s_csMapMutex;
static std::unordered_map<uint32_t, std::unique_ptr<std::recursive_mutex>> s_cs;

static std::recursive_mutex& CriticalSection(uint32_t addr)
{
    std::lock_guard l(s_csMapMutex);
    auto& p = s_cs[addr];
    if (!p) p = std::make_unique<std::recursive_mutex>();
    return *p;
}

KFUNC(RtlInitializeCriticalSection) { CriticalSection(ARG(3)); }
KFUNC(RtlInitializeCriticalSectionAndSpinCount) { CriticalSection(ARG(3)); RET(STATUS_SUCCESS); }
KFUNC(RtlEnterCriticalSection) { CriticalSection(ARG(3)).lock(); }
KFUNC(RtlLeaveCriticalSection) { CriticalSection(ARG(3)).unlock(); }
KFUNC(RtlTryEnterCriticalSection) { RET(CriticalSection(ARG(3)).try_lock() ? 1 : 0); }

static std::recursive_mutex s_spin;
KFUNC(KfAcquireSpinLock) { s_spin.lock(); RET(0); }
KFUNC(KfReleaseSpinLock) { s_spin.unlock(); }
KFUNC(KeAcquireSpinLockAtRaisedIrql) { s_spin.lock(); }
KFUNC(KeReleaseSpinLockFromRaisedIrql) { s_spin.unlock(); }
KFUNC(KeRaiseIrqlToDpcLevel) { RET(0); }
KFUNC(KfLowerIrql) {}
KFUNC(KeEnterCriticalRegion) {}
KFUNC(KeLeaveCriticalRegion) {}
KFUNC(KeLockL2) {}
KFUNC(KeUnlockL2) {}

KFUNC(ExInitializeReadWriteLock) { CriticalSection(ARG(3)); }
KFUNC(ExAcquireReadWriteLockExclusive) { CriticalSection(ARG(3)).lock(); }
KFUNC(ExAcquireReadWriteLockShared) { CriticalSection(ARG(3)).lock(); }
KFUNC(ExReleaseReadWriteLock) { CriticalSection(ARG(3)).unlock(); }

// ---------------------------------------------------------------- TLS

static std::atomic<uint32_t> s_tlsNext = 0;
static thread_local uint32_t t_tls[64];

KFUNC(KeTlsAlloc) { RET(s_tlsNext++); }
KFUNC(KeTlsFree) { RET(1); }
KFUNC(KeTlsGetValue) { RET(ARG(3) < 64 ? t_tls[ARG(3)] : 0); }
KFUNC(KeTlsSetValue) { if (ARG(3) < 64) t_tls[ARG(3)] = ARG(4); RET(1); }

// ---------------------------------------------------------------- événements, sémaphores, attente

KFUNC(NtCreateEvent)   // (pHandle, objAttr, eventType, initialState)
{
    auto ev = std::make_shared<KEvent>();
    ev->manualReset = ARG(5) == 0;
    ev->signaled = ARG(6) != 0;
    Wr32(ARG(3), NewHandle(ev));
    RET(STATUS_SUCCESS);
}
static void SyncLog(const char* what, uint32_t id, uint32_t lr) { static int n; if (getenv("IKARUGA_SYNCLOG") && n++ < 200) KLOG("%s %08X (depuis %08X)", what, id, lr); }
KFUNC(NtSetEvent) { SyncLog("NtSetEvent", ARG(3), uint32_t(ctx.lr)); if (auto o = FromHandle(ARG(3))) o->Signal(); RET(STATUS_SUCCESS); }
KFUNC(NtClearEvent) { if (auto e = std::dynamic_pointer_cast<KEvent>(FromHandle(ARG(3)))) e->Reset(); RET(STATUS_SUCCESS); }
KFUNC(KeSetEvent) { SyncLog("KeSetEvent", ARG(3), uint32_t(ctx.lr)); FromDispatcher(ARG(3))->Signal(); RET(0); }
KFUNC(KeResetEvent)
{
    auto o = FromDispatcher(ARG(3));
    if (auto g = std::dynamic_pointer_cast<KGuestDispatcher>(o)) g->Reset();
    else if (auto e = std::dynamic_pointer_cast<KEvent>(o)) e->Reset();
    RET(0);
}

KFUNC(KeInitializeSemaphore)   // (sem, count, limit)
{
    Wr8(ARG(3), 5); Wr32(ARG(3) + 4, ARG(4)); Wr32(ARG(3) + 0x10, ARG(5));
}
KFUNC(KeReleaseSemaphore)   // (sem, increment, adjustment, wait)
{
    auto sem = std::dynamic_pointer_cast<KGuestDispatcher>(FromDispatcher(ARG(3)));
    RET(sem ? sem->Release(ARG(5)) : 0);
}

// Attente sans délai : par tranches de 2 s, en signalant les attentes longues (débogage des blocages)
static bool WaitLogged(KObject& o, int64_t timeoutMs, const char* what, uint32_t id, uint32_t lr)
{
    if (timeoutMs >= 0) return o.Wait(timeoutMs);
    for (int slices = 0;; slices++)
    {
        if (o.Wait(2000)) return true;
        if (slices == 0) KLOG("attente longue : %s %08X (retour vers %08X)", what, id, lr);
    }
}

KFUNC(KeWaitForSingleObject)   // (object, reason, mode, alertable, pTimeout)
{
    RET(WaitLogged(*FromDispatcher(ARG(3)), TimeoutMs(ARG(7)), "objet", ARG(3), uint32_t(ctx.lr)) ? STATUS_SUCCESS : STATUS_TIMEOUT);
}
KFUNC(NtWaitForSingleObjectEx)   // (handle, mode, alertable, pTimeout)
{
    auto o = FromHandle(ARG(3));
    if (!o) RET(STATUS_INVALID_HANDLE);
    RET(WaitLogged(*o, TimeoutMs(ARG(6)), "handle", ARG(3), uint32_t(ctx.lr)) ? STATUS_SUCCESS : STATUS_TIMEOUT);
}

// Attente multiple simplifiée : interrogation périodique
static uint32_t WaitMultiple(std::vector<std::shared_ptr<KObject>> objs, bool waitAll, int64_t timeoutMs)
{
    auto start = std::chrono::steady_clock::now();
    std::vector<bool> done(objs.size());
    for (;;)
    {
        size_t count = 0;
        for (size_t i = 0; i < objs.size(); i++)
        {
            if (!done[i] && objs[i]->Wait(0)) done[i] = true;
            if (done[i]) { if (!waitAll) return uint32_t(i); count++; }
        }
        if (waitAll && count == objs.size()) return STATUS_SUCCESS;
        if (timeoutMs >= 0 && std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(timeoutMs)) return STATUS_TIMEOUT;
        if (timeoutMs < 0 && std::chrono::steady_clock::now() - start >= std::chrono::seconds(2))
        {
            static int logged;
            if (logged++ < 5) KLOG("attente longue : %zu objets (tous=%d)", objs.size(), waitAll);
            start = std::chrono::steady_clock::now() + std::chrono::hours(1000);
        }
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
}

KFUNC(NtWaitForMultipleObjectsEx)   // (count, handles, waitType, mode, alertable, pTimeout)
{
    std::vector<std::shared_ptr<KObject>> objs;
    for (uint32_t i = 0; i < ARG(3); i++) objs.push_back(FromHandle(Rd32(ARG(4) + i * 4)));
    RET(WaitMultiple(objs, ARG(5) == 0, TimeoutMs(ARG(8))));
}
KFUNC(KeWaitForMultipleObjects)   // (count, objects, waitType, reason, mode, alertable, pTimeout, waitBlocks)
{
    std::vector<std::shared_ptr<KObject>> objs;
    for (uint32_t i = 0; i < ARG(3); i++) objs.push_back(FromDispatcher(Rd32(ARG(4) + i * 4)));
    RET(WaitMultiple(objs, ARG(5) == 0, TimeoutMs(ARG(9))));
}

KFUNC(NtClose)
{
    std::lock_guard l(s_objMutex);
    s_handles.erase(ARG(3));
    RET(STATUS_SUCCESS);
}
KFUNC(NtDuplicateObject)   // (handle, pNewHandle, options)
{
    auto o = FromHandle(ARG(3));
    if (!o) RET(STATUS_INVALID_HANDLE);
    Wr32(ARG(4), NewHandle(o));
    RET(STATUS_SUCCESS);
}
KFUNC(ObReferenceObjectByHandle)   // (handle, type, pObject)
{
    auto t = std::dynamic_pointer_cast<KThread>(FromHandle(ARG(3)));
    Wr32(ARG(5), t ? t->guestThread : ARG(3));
    RET(STATUS_SUCCESS);
}
KFUNC(ObDereferenceObject) {}
KFUNC(ObReferenceObject) {}

// ---------------------------------------------------------------- threads

static std::mutex s_threadsMutex;
static std::unordered_map<uint32_t, std::shared_ptr<KThread>> s_threadsByGuest;   // KTHREAD invité -> thread

void DebugRegisterThread(const char* name);

static void ThreadMain(std::shared_ptr<KThread> t)
{
    {
        std::unique_lock l(t->suspendMutex);
        t->suspendCv.wait(l, [&] { return !t->suspended; });
    }
    PPCContext& ctx = t->ctx;
    {
        char name[32]; snprintf(name, sizeof name, "thread %08X", t->startCtx ? t->startCtx : t->startAddr);
        DebugRegisterThread(name);
    }
    KLOG("thread %08X démarré (KTHREAD %08X)", t->startAddr, t->guestThread);
    if (t->xapiStartup)
    {
        ctx.r3.u64 = t->startAddr; ctx.r4.u64 = t->startCtx;
        GuestCall(ctx, t->xapiStartup);
    }
    else
    {
        ctx.r3.u64 = t->startCtx;
        GuestCall(ctx, t->startAddr);
    }
    t->Signal();
}

KFUNC(ExCreateThread)   // (pHandle, stackSize, pThreadId, xapiStartup, startAddr, startCtx, flags)
{
    auto t = std::make_shared<KThread>();
    t->stackSize = ARG(4); t->xapiStartup = ARG(6); t->startAddr = ARG(7); t->startCtx = ARG(8);
    t->suspended = ARG(9) & 1;
    KLOG("ExCreateThread start=%08X ctx=%08X xapi=%08X flags=%X", t->startAddr, t->startCtx, t->xapiStartup, ARG(9));
    GuestInitThreadContext(t->ctx, t->stackSize);
    t->guestThread = Rd32(t->ctx.r13.u32 + 0x100);
    { std::lock_guard l(s_objMutex); s_threadDispatchers[t->guestThread] = t; }
    uint32_t h = NewHandle(t);
    if (ARG(3)) Wr32(ARG(3), h);
    if (ARG(5)) Wr32(ARG(5), Rd32(t->guestThread + 0x14C));   // identifiant du thread
    std::thread(ThreadMain, t).detach();
    RET(STATUS_SUCCESS);
}

KFUNC(KeResumeThread)
{
    std::shared_ptr<KThread> t;
    {
        std::lock_guard l(s_objMutex);
        for (auto& [h, o] : s_handles)
            if (auto k = std::dynamic_pointer_cast<KThread>(o); k && (k->guestThread == ARG(3) || h == ARG(3))) t = k;
    }
    KLOG("KeResumeThread(%08X) -> %s", ARG(3), t ? "trouvé" : "inconnu");
    if (t) { std::lock_guard l(t->suspendMutex); t->suspended = false; t->suspendCv.notify_all(); }
    RET(0);
}

KFUNC(ExTerminateThread) { KLOG("ExTerminateThread(%u)", ARG(3)); }
KFUNC(KeSetBasePriorityThread) { RET(0); }
KFUNC(KeSetAffinityThread) { RET(0); }
KFUNC(KeGetCurrentProcessType) { RET(1); }   // processus titre
KFUNC(KeDelayExecutionThread)   // (mode, alertable, pInterval)
{
    int64_t ms = TimeoutMs(ARG(5));
    if (ms <= 0) std::this_thread::yield(); else std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    RET(STATUS_SUCCESS);
}
KFUNC(KeEnableFpuExceptions) {}
KFUNC(KiApcNormalRoutineNop) {}

// ---------------------------------------------------------------- temps

KFUNC(KeQueryPerformanceFrequency) { ctx.r3.u64 = 50000000; }
KFUNC(KeQuerySystemTime)
{
    auto now = std::chrono::system_clock::now().time_since_epoch();
    uint64_t ft = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count() / 100 + 116444736000000000ull;
    Wr64(ARG(3), ft);
}

// ---------------------------------------------------------------- chaînes

KFUNC(RtlInitAnsiString)   // (pString, src)
{
    uint16_t len = ARG(4) ? (uint16_t)strlen((char*)g_base + ARG(4)) : 0;
    Wr16(ARG(3), len); Wr16(ARG(3) + 2, ARG(4) ? len + 1 : 0); Wr32(ARG(3) + 4, ARG(4));
}
KFUNC(RtlNtStatusToDosError) { RET(ARG(3) == 0 ? 0 : 31); }

// ---------------------------------------------------------------- fichiers (game:\ -> extracted/)

static bool EqualNoCase(const std::string& a, const std::string& b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return tolower(x) == tolower(y); });
}

static std::filesystem::path ResolvePath(std::string p)
{
    for (auto& c : p) if (c == '\\') c = '/';
    if (auto colon = p.find(':'); colon != std::string::npos) p = p.substr(colon + 1);
    for (const char* prefix : { "/Device/Cdrom0", "/Device/Harddisk0/Partition1", "/??/game:" })
        if (p.rfind(prefix, 0) == 0) p = p.substr(strlen(prefix));
    while (!p.empty() && p[0] == '/') p.erase(0, 1);
    // le système de fichiers Xbox ignore la casse
    std::filesystem::path result = g_gameDir;
    for (const auto& part : std::filesystem::path(p))
    {
        std::filesystem::path next = result / part;
        if (!std::filesystem::exists(next) && std::filesystem::is_directory(result))
            for (auto& e : std::filesystem::directory_iterator(result))
                if (EqualNoCase(e.path().filename().string(), part.string())) { next = e.path(); break; }
        result = next;
    }
    return result;
}

static std::string ObjectName(uint32_t objAttr)
{
    uint32_t str = Rd32(objAttr + 4);
    return std::string((char*)g_base + Rd32(str + 4), Rd16(str));
}

static uint32_t OpenFile(uint32_t pHandle, uint32_t objAttr, uint32_t iosb)
{
    std::string name = ObjectName(objAttr);
    auto path = ResolvePath(name);
    auto file = std::make_shared<KFile>();
    file->path = path;
    file->isDir = std::filesystem::is_directory(path);
    if (!file->isDir && !(file->f = fopen(path.string().c_str(), "rb")))
    {
        KLOG("fichier introuvable : %s", name.c_str());
        if (iosb) Wr32(iosb, STATUS_OBJECT_NAME_NOT_FOUND);
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    KLOG("ouverture %s", name.c_str());
    Wr32(pHandle, NewHandle(file));
    if (iosb) { Wr32(iosb, 0); Wr32(iosb + 4, 1); }
    return STATUS_SUCCESS;
}

KFUNC(NtCreateFile) { RET(OpenFile(ARG(3), ARG(5), ARG(6))); }   // (pHandle, access, objAttr, iosb, ...)
KFUNC(NtOpenFile) { RET(OpenFile(ARG(3), ARG(5), ARG(6))); }

KFUNC(NtReadFile)   // (handle, event, apc, apcCtx, iosb, buffer, length, pOffset)
{
    auto file = std::dynamic_pointer_cast<KFile>(FromHandle(ARG(3)));
    if (!file || !file->f) RET(STATUS_INVALID_HANDLE);
    if (ARG(10)) fseek(file->f, (long)Rd64(ARG(10)), SEEK_SET);
    size_t n = fread(g_base + ARG(8), 1, ARG(9), file->f);
    Wr32(ARG(7), n ? STATUS_SUCCESS : STATUS_END_OF_FILE); Wr32(ARG(7) + 4, (uint32_t)n);
    if (ARG(4)) if (auto ev = FromHandle(ARG(4))) ev->Signal();
    static int logged;
    if (getenv("IKARUGA_IOLOG") && logged++ < 40)
        KLOG("NtReadFile %s : %u octets à %lld -> %zu (event %08X, apc %08X, ctx %08X)", file->path.filename().string().c_str(),
            ARG(9), ARG(10) ? (long long)Rd64(ARG(10)) : -1ll, n, ARG(4), ARG(5), ARG(6));
    RET(n ? STATUS_SUCCESS : STATUS_END_OF_FILE);
}

KFUNC(NtQueryInformationFile)   // (handle, iosb, info, length, class)
{
    auto file = std::dynamic_pointer_cast<KFile>(FromHandle(ARG(3)));
    if (!file) RET(STATUS_INVALID_HANDLE);
    uint64_t size = file->isDir ? 0 : std::filesystem::file_size(file->path);
    switch (ARG(7))
    {
    case 5:  // FileStandardInformation
        Wr64(ARG(5), (size + 0xFFF) & ~0xFFFull); Wr64(ARG(5) + 8, size); Wr32(ARG(5) + 16, 1); Wr8(ARG(5) + 20, 0); Wr8(ARG(5) + 21, file->isDir);
        break;
    case 14: // FilePositionInformation
        Wr64(ARG(5), file->f ? ftell(file->f) : 0);
        break;
    case 34: // FileNetworkOpenInformation
        memset(g_base + ARG(5), 0, 0x38); Wr64(ARG(5) + 0x20, (size + 0xFFF) & ~0xFFFull); Wr64(ARG(5) + 0x28, size); Wr32(ARG(5) + 0x30, file->isDir ? 0x10 : 0x80);
        break;
    default:
        KLOG("NtQueryInformationFile classe %u non gérée", ARG(7));
    }
    Wr32(ARG(4), STATUS_SUCCESS); Wr32(ARG(4) + 4, ARG(6));
    RET(STATUS_SUCCESS);
}

KFUNC(NtSetInformationFile)   // (handle, iosb, info, length, class)
{
    auto file = std::dynamic_pointer_cast<KFile>(FromHandle(ARG(3)));
    if (file && file->f && ARG(7) == 14) fseek(file->f, (long)Rd64(ARG(5)), SEEK_SET);
    RET(STATUS_SUCCESS);
}

KFUNC(NtQueryFullAttributesFile)   // (objAttr, info)
{
    auto path = ResolvePath(ObjectName(ARG(3)));
    if (!std::filesystem::exists(path)) RET(STATUS_OBJECT_NAME_NOT_FOUND);
    bool dir = std::filesystem::is_directory(path);
    uint64_t size = dir ? 0 : std::filesystem::file_size(path);
    memset(g_base + ARG(4), 0, 0x38);
    Wr64(ARG(4) + 0x20, (size + 0xFFF) & ~0xFFFull); Wr64(ARG(4) + 0x28, size); Wr32(ARG(4) + 0x30, dir ? 0x10 : 0x80);
    RET(STATUS_SUCCESS);
}

KFUNC(NtQueryVolumeInformationFile) { RET(STATUS_UNSUCCESSFUL); }
KFUNC(NtFlushBuffersFile) { RET(STATUS_SUCCESS); }

// ---------------------------------------------------------------- XEX / système

KFUNC(XexCheckExecutablePrivilege) { RET(0); }
KFUNC(XGetLanguage) { RET(2); }      // français
KFUNC(XGetGameRegion) { RET(0xFF); }
KFUNC(XGetAVPack) { RET(6); }        // HDMI
static void FillVideoMode(uint32_t p);
KFUNC(XGetVideoMode) { FillVideoMode(ARG(3)); }
KFUNC(XamGetSystemVersion) { RET(0x20000000); }
KFUNC(ExGetXConfigSetting) { if (ARG(5)) memset(g_base + ARG(5), 0, ARG(6)); RET(STATUS_SUCCESS); }
KFUNC(DbgPrint) { KLOG("DbgPrint: %s", (char*)g_base + ARG(3)); }
KFUNC(DbgBreakPoint) { KLOG("DbgBreakPoint"); }
KFUNC(KeBugCheck) { KLOG("KeBugCheck(%08X)", ARG(3)); abort(); }
KFUNC(KeBugCheckEx) { KLOG("KeBugCheckEx(%08X, %08X, %08X)", ARG(3), ARG(4), ARG(5)); abort(); }
KFUNC(HalReturnToFirmware) { KLOG("HalReturnToFirmware(%u)", ARG(3)); exit(0); }
KFUNC(XamLoaderTerminateTitle) { KLOG("XamLoaderTerminateTitle"); exit(0); }

// ---------------------------------------------------------------- manettes (SDL3)

static SDL_Gamepad* Pad(uint32_t index)
{
    static SDL_Gamepad* pads[4];
    if (index >= 4) return nullptr;
    if (!pads[index])
    {
        int count = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&count);
        if (ids && (int)index < count) pads[index] = SDL_OpenGamepad(ids[index]);
        SDL_free(ids);
    }
    return pads[index];
}

// Clavier = manette virtuelle du joueur 1 (toujours connectée) : Entrée=START, Échap=BACK, flèches=croix,
// W/X/C/V=A/B/X/Y (disposition AZERTY : touches du bas à gauche). IKARUGA_AUTOSTART=n appuie sur START
// toutes les n images (tests sans clavier).
static uint16_t KeyboardButtons()
{
    const bool* k = SDL_GetKeyboardState(nullptr);
    uint16_t b = 0;
    if (k)
    {
        if (k[SDL_SCANCODE_UP]) b |= 0x1;
        if (k[SDL_SCANCODE_DOWN]) b |= 0x2;
        if (k[SDL_SCANCODE_LEFT]) b |= 0x4;
        if (k[SDL_SCANCODE_RIGHT]) b |= 0x8;
        if (k[SDL_SCANCODE_RETURN]) b |= 0x10;
        if (k[SDL_SCANCODE_ESCAPE]) b |= 0x20;
        if (k[SDL_SCANCODE_Z]) b |= 0x1000;   // touche "W" en AZERTY
        if (k[SDL_SCANCODE_X]) b |= 0x2000;
        if (k[SDL_SCANCODE_C]) b |= 0x4000;
        if (k[SDL_SCANCODE_V]) b |= 0x8000;
    }
    static const char* autoStart = getenv("IKARUGA_AUTOSTART");
    if (autoStart)
    {
        static uint32_t calls;
        uint32_t period = std::max(1, atoi(autoStart)) * 4;
        if (calls++ % period < 4) b |= 0x10 | 0x1000;   // START + A
    }
    return b;
}

KFUNC(XamInputGetCapabilities)   // (user, flags, caps)
{
    if (!Pad(ARG(3)) && ARG(3) != 0) RET(1167);   // ERROR_DEVICE_NOT_CONNECTED (le clavier tient lieu de manette 1)
    memset(g_base + ARG(5), 0, 0x14);
    Wr8(ARG(5), 1); Wr8(ARG(5) + 1, 1);   // XINPUT_DEVTYPE_GAMEPAD / SUBTYPE_GAMEPAD
    RET(0);
}

KFUNC(XamInputGetState)   // (user, flags, state)
{
    SDL_Gamepad* p = Pad(ARG(3));
    static std::atomic<uint32_t> packet;
    uint32_t s = ARG(5);
    if (!p)
    {
        if (ARG(3) != 0) RET(1167);
        memset(g_base + s, 0, 16);
        Wr32(s, ++packet);
        Wr16(s + 4, KeyboardButtons());
        RET(0);
    }
    uint16_t b = KeyboardButtons();
    struct { SDL_GamepadButton sdl; uint16_t x; } map[] = {
        { SDL_GAMEPAD_BUTTON_DPAD_UP, 0x1 }, { SDL_GAMEPAD_BUTTON_DPAD_DOWN, 0x2 }, { SDL_GAMEPAD_BUTTON_DPAD_LEFT, 0x4 },
        { SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0x8 }, { SDL_GAMEPAD_BUTTON_START, 0x10 }, { SDL_GAMEPAD_BUTTON_BACK, 0x20 },
        { SDL_GAMEPAD_BUTTON_LEFT_STICK, 0x40 }, { SDL_GAMEPAD_BUTTON_RIGHT_STICK, 0x80 }, { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0x100 },
        { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0x200 }, { SDL_GAMEPAD_BUTTON_GUIDE, 0x400 }, { SDL_GAMEPAD_BUTTON_SOUTH, 0x1000 },
        { SDL_GAMEPAD_BUTTON_EAST, 0x2000 }, { SDL_GAMEPAD_BUTTON_WEST, 0x4000 }, { SDL_GAMEPAD_BUTTON_NORTH, 0x8000 } };
    for (auto& m : map) if (SDL_GetGamepadButton(p, m.sdl)) b |= m.x;
    auto axis = [&](SDL_GamepadAxis a, bool invert) { int v = SDL_GetGamepadAxis(p, a); return uint16_t(invert ? std::min(32767, -v) : v); };
    Wr32(s, ++packet);
    Wr16(s + 4, b);
    Wr8(s + 6, SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) >> 7);
    Wr8(s + 7, SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) >> 7);
    Wr16(s + 8, axis(SDL_GAMEPAD_AXIS_LEFTX, false));
    Wr16(s + 10, axis(SDL_GAMEPAD_AXIS_LEFTY, true));
    Wr16(s + 12, axis(SDL_GAMEPAD_AXIS_RIGHTX, false));
    Wr16(s + 14, axis(SDL_GAMEPAD_AXIS_RIGHTY, true));
    RET(0);
}
KFUNC(XamInputSetState) { RET(0); }

// ---------------------------------------------------------------- profil / Live (hors ligne)

KFUNC(XamUserGetSigninState) { RET(ARG(3) == 0 ? 1 : 0); }   // joueur 1 connecté localement
KFUNC(XamUserGetXUID) { if (ARG(4)) Wr64(ARG(4), 0xE000000000000001ull); RET(0); }
KFUNC(XamUserGetName) { strncpy((char*)g_base + ARG(4), "Joueur", ARG(5)); RET(0); }
KFUNC(XamUserCheckPrivilege) { if (ARG(5)) Wr32(ARG(5), 1); RET(0); }
// Notifications système : file par écouteur ; comme Xenia, le premier écouteur système reçoit au
// démarrage XN_SYS_UI (1 puis 0) et deux XN_SYS_SIGNINCHANGED, que certains jeux attendent.
struct KNotifyListener : KEvent
{
    std::deque<std::pair<uint32_t, uint32_t>> queue;   // (identifiant, paramètre)
    uint64_t mask = 0;
};

KFUNC(XamNotifyCreateListener)   // (masque 64 bits, version max)
{
    auto l = std::make_shared<KNotifyListener>();
    l->manualReset = true;
    l->mask = ctx.r3.u64;
    static bool startupSent;
    if (!startupSent && (l->mask & 1))   // XNOTIFY_SYSTEM
    {
        startupSent = true;
        l->queue = { { 0x00000009, 1 }, { 0x00000009, 0 }, { 0x0000000A, 1 }, { 0x0000000A, 1 } };
        l->Signal();
    }
    KLOG("XamNotifyCreateListener masque=%llx", (unsigned long long)l->mask);
    RET(NewHandle(l));
}

KFUNC(XNotifyGetNext)   // (handle, idCherché, pId, pParam)
{
    if (ARG(6)) Wr32(ARG(6), 0);
    if (!ARG(5)) RET(0);
    Wr32(ARG(5), 0);
    auto l = std::dynamic_pointer_cast<KNotifyListener>(FromHandle(ARG(3)));
    if (!l) RET(0);
    std::lock_guard lock(l->m);
    for (auto it = l->queue.begin(); it != l->queue.end(); ++it)
        if (!ARG(4) || it->first == ARG(4))
        {
            Wr32(ARG(5), it->first);
            if (ARG(6)) Wr32(ARG(6), it->second);
            KLOG("XNotifyGetNext -> %08X (%u)", it->first, it->second);
            l->queue.erase(it);
            if (l->queue.empty()) l->signaled = false;
            return void(ctx.r3.u64 = 1);
        }
    RET(0);
}
KFUNC(XamContentGetDeviceState) { RET(0); }
KFUNC(XamVoiceHeadsetPresent) { RET(0); }
KFUNC(NetDll_XNetStartup) { RET(0); }
KFUNC(NetDll_WSAStartup) { RET(0); }
KFUNC(NetDll_XNetGetTitleXnAddr) { RET(4); }   // XNET_GET_XNADDR_NONE

// ---------------------------------------------------------------- vidéo (Vd*) : en attendant le vrai GPU

static void FillVideoMode(uint32_t p)
{
    memset(g_base + p, 0, 0x30);
    Wr32(p, 1280); Wr32(p + 4, 720);           // largeur, hauteur
    Wr32(p + 8, 0); Wr32(p + 12, 1);           // entrelacé, 16:9
    Wr32(p + 16, 1); Wr32(p + 20, 0x42700000); // haute définition, 60.0f
    Wr32(p + 24, 1);                           // standard vidéo NTSC
    Wr32(p + 28, 0x4A); Wr32(p + 32, 1);       // valeurs fixes du noyau (diviseur utilisé par le jeu)
}

KFUNC(VdQueryVideoMode) { FillVideoMode(ARG(3)); }
KFUNC(VdQueryVideoFlags) { RET(7); }   // 16:9 | HD | 1080i possible
KFUNC(VdGetCurrentDisplayInformation)   // X_DISPLAY_INFO (0x58 octets)
{
    uint32_t p = ARG(3);
    memset(g_base + p, 0, 0x58);
    Wr16(p, 1280); Wr16(p + 2, 720);                      // tampon avant
    Wr32(p + 0x08 + 0x08, 1280); Wr32(p + 0x08 + 0x0C, 720); // rectangle source du scaler (x2, y2)
    Wr32(p + 0x08 + 0x10, 1280); Wr32(p + 0x08 + 0x14, 720); // sortie du scaler
    Wr32(p + 0x08 + 0x18, 1); Wr32(p + 0x08 + 0x28, 1);    // filtres vertical / horizontal
    Wr16(p + 0x40, 320); Wr16(p + 0x42, 180); Wr16(p + 0x44, 320); Wr16(p + 0x46, 180);   // overscan
    Wr16(p + 0x48, 1280); Wr16(p + 0x4A, 720);
    Wr32(p + 0x4C, 0x42700000);                           // 60.0f
    Wr16(p + 0x56, 1280);
}
KFUNC(VdGetCurrentDisplayGamma) { Wr32(ARG(3), 2); Wr32(ARG(4), 0x400CCCCD); }   // sRGB, 2.2f
KFUNC(VdIsHSIOTrainingSucceeded) { RET(1); }
KFUNC(VdInitializeEngines) { RET(1); }
KFUNC(VdSetDisplayMode) { RET(0); }
KFUNC(VdRetrainEDRAM) { RET(0); }
KFUNC(VdRetrainEDRAMWorker) {}
KFUNC(VdCallGraphicsNotificationRoutines) { RET(0); }
KFUNC(VdEnableDisableClockGating) {}
KFUNC(VdPersistDisplay) { if (ARG(4)) Wr32(ARG(4), 0); RET(0); }
KFUNC(VdShutdownEngines) {}
KFUNC(VdInitializeScalerCommandBuffer) { RET(0); }

KFUNC(VdGetSystemCommandBuffer)   // (pInfo 0x94 octets, pValue) : comme Xenia, structure remise à zéro
{
    memset(g_base + ARG(3), 0, 0x94);
    Wr32(ARG(4), 0xBEEF0000);
}
KFUNC(VdSetSystemCommandBufferGpuIdentifierAddress) {}

// Anneau de commandes GPU : traité par le processeur de commandes de gpu.cpp
void GpuInitializeRingBuffer(uint32_t base, uint32_t sizeLog2);
void GpuEnableReadPointerWriteBack(uint32_t addr);
void GpuSetInterruptCallback(uint32_t cb, uint32_t ctx);
void GpuOnSwap();
extern std::recursive_mutex g_interruptMutex;
void VulkanPresent(float r, float g, float b);
void GpuPrintStats();
KFUNC(VdInitializeRingBuffer) { GpuInitializeRingBuffer(ARG(3), ARG(4)); }
KFUNC(VdEnableRingBufferRPtrWriteBack) { GpuEnableReadPointerWriteBack(ARG(3)); }

// Interruption verticale (60 Hz) : appelle le callback graphique du jeu
static std::atomic<uint32_t> s_gfxCallback, s_gfxCallbackCtx;
KFUNC(VdSetGraphicsInterruptCallback)
{
    s_gfxCallback = ARG(3); s_gfxCallbackCtx = ARG(4);
    GpuSetInterruptCallback(ARG(3), ARG(4));
    static std::once_flag once;
    std::call_once(once, [] {
        std::thread([] {
            PPCContext ctx;
            GuestInitThreadContext(ctx, 0x10000);
            auto next = std::chrono::steady_clock::now();
            for (;;)
            {
                next += std::chrono::microseconds(16667);
                std::this_thread::sleep_until(next);
                if (uint32_t cb = s_gfxCallback)
                {
                    std::lock_guard lock(g_interruptMutex);   // sérialisé avec les interruptions du GPU
                    Wr8(ctx.r13.u32 + 0x10C, 2);   // Xenia livre le vblank sur le processeur 2
                    ctx.r3.u64 = 0;   // source : vblank
                    ctx.r4.u64 = s_gfxCallbackCtx;
                    GuestCall(ctx, cb);
                }
            }
        }).detach();
    });
}

static std::atomic<uint64_t> s_frames;
KFUNC(VdSwap)   // (cmdBuffer, ...) : le noyau remplit 64 dwords de commandes ; des NOP suffisent sans GPU
{
    // Paquet type 3 opcode 0x64 couvrant les 64 dwords : le thread GPU présentera à ce point du flux
    Wr32(ARG(3), 0xC0000000 | (62u << 16) | (0x64u << 8));
    for (uint32_t i = 1; i < 64; i++) Wr32(ARG(3) + i * 4, 0);
    GpuOnSwap();
    if (s_frames % 120 == 119) GpuPrintStats();
    if (++s_frames % 60 == 1) KLOG("VdSwap : image %llu", (unsigned long long)s_frames.load());
    RET(0);
}

// ---------------------------------------------------------------- C runtime exporté par le noyau

KFUNC(_vsnprintf)   // (buffer, count, format, va_list) : formatage minimal façon PPC (args 64 bits sur la pile)
{
    std::string out;
    const char* f = (const char*)g_base + ARG(5);
    uint32_t va = ARG(6);
    auto next = [&]() { uint64_t v = Rd64(va); va += 8; return v; };
    for (; *f; f++)
    {
        if (*f != '%') { out += *f; continue; }
        std::string spec = "%";
        while (*++f && strchr("-+ #0123456789.lhz", *f)) if (*f != 'l' && *f != 'h' && *f != 'z') spec += *f;
        char buf[512];
        switch (*f)
        {
        case 'd': case 'i': snprintf(buf, sizeof buf, (spec + "lld").c_str(), (long long)(int32_t)next()); break;
        case 'u': case 'x': case 'X': case 'o': snprintf(buf, sizeof buf, (spec + "ll" + *f).c_str(), (unsigned long long)(uint32_t)next()); break;
        case 'f': case 'g': case 'e': { uint64_t b = next(); double d; memcpy(&d, &b, 8); snprintf(buf, sizeof buf, (spec + *f).c_str(), d); break; }
        case 'c': buf[0] = (char)next(); buf[1] = 0; break;
        case 's': { uint32_t p = (uint32_t)next(); snprintf(buf, sizeof buf, (spec + "s").c_str(), p ? (char*)g_base + p : "(null)"); break; }
        case 'p': snprintf(buf, sizeof buf, "%08X", (uint32_t)next()); break;
        case '%': strcpy(buf, "%"); break;
        default: buf[0] = 0;
        }
        out += buf;
    }
    uint32_t n = std::min<uint32_t>(out.size(), ARG(4) ? ARG(4) - 1 : 0);
    memcpy(g_base + ARG(3), out.data(), n);
    if (ARG(4)) g_base[ARG(3) + n] = 0;
    RET(n);
}

// ---------------------------------------------------------------- audio (XAudio) : pilote de rendu
// Le jeu enregistre un callback que le système appelle à chaque trame consommée (256 échantillons,
// 48 kHz -> 5,33 ms). Sans ces appels, son moteur audio (et les écrans qui attendent un son) se bloque.
// Les échantillons soumis sont pour l'instant ignorés.

static std::atomic<uint32_t> s_audioCallback, s_audioCallbackArg;
static std::atomic<bool> s_audioRunning;

KFUNC(XAudioRegisterRenderDriverClient)   // (callbackPtr -> {fonction, argument}, pDriver)
{
    s_audioCallback = Rd32(ARG(3));
    s_audioCallbackArg = Rd32(ARG(3) + 4);
    Wr32(ARG(4), 0x41550000);
    KLOG("XAudio : client enregistré (callback %08X, argument %08X)", s_audioCallback.load(), s_audioCallbackArg.load());
    if (!s_audioRunning.exchange(true))
        std::thread([] {
            PPCContext ctx;
            GuestInitThreadContext(ctx, 0x10000);
            auto next = std::chrono::steady_clock::now();
            while (s_audioRunning)
            {
                next += std::chrono::microseconds(5333);
                std::this_thread::sleep_until(next);
                if (uint32_t cb = s_audioCallback)
                {
                    ctx.r3.u64 = s_audioCallbackArg;
                    GuestCall(ctx, cb);
                }
            }
        }).detach();
    RET(0);
}
KFUNC(XAudioUnregisterRenderDriverClient) { s_audioCallback = 0; RET(0); }
KFUNC(XAudioSubmitRenderDriverFrame) { RET(0); }
KFUNC(XAudioGetVoiceCategoryVolume) { if (ARG(4)) Wr32(ARG(4), 0x3F800000); RET(0); }   // 1.0f
KFUNC(XAudioGetVoiceCategoryVolumeChangeMask) { if (ARG(4)) Wr32(ARG(4), 0); RET(0); }

// Base de temps PowerPC (instruction mftb) : 50 MHz comme sur Xbox 360, cohérente avec
// KeQueryPerformanceFrequency (les jeux n'en lisent souvent que 32 bits)
extern "C" uint64_t GuestTimebase()
{
    static const auto start = std::chrono::steady_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
    return uint64_t(ns) / 20;
}

KFUNC(XamUserGetSigninInfo)   // (user, flags, XUSER_SIGNIN_INFO*) : joueur 1 connecté localement
{
    memset(g_base + ARG(5), 0, 0x28);
    if (ARG(3) != 0) RET(0x525);   // ERROR_NO_SUCH_USER
    Wr64(ARG(5), 0xE000000000000001ull);    // xuid
    Wr32(ARG(5) + 0xC, 1);                  // XUserSigninState_SignedInLocally
    strcpy((char*)g_base + ARG(5) + 0x18, "Joueur");
    RET(0);
}

KFUNC(XamContentGetLicenseMask)   // (pMask, overlapped) : jeu acheté (licence complète)
{
    Wr32(ARG(3), 0xFFFFFFFF);
    RET(0);
}

// Fin immédiate d'une opération XAM asynchrone (XAM_OVERLAPPED : résultat, longueur, contexte, event)
static uint32_t CompleteOverlapped(uint32_t ov, uint32_t result, uint32_t length = 0)
{
    Wr32(ov, result);
    Wr32(ov + 4, length);
    Wr32(ov + 0x14, result);   // erreur étendue
    if (uint32_t ev = Rd32(ov + 0xC))
    {
        if (auto o = FromHandle(ev)) o->Signal();
        else FromDispatcher(ev)->Signal();
    }
    return 0x3E5;   // ERROR_IO_PENDING
}

// Réglages du profil : on répond "non défini" pour chaque réglage, le jeu prend alors ses valeurs par défaut
KFUNC(XamUserReadProfileSettings)   // (titre, joueur, nbXuid, xuids, nb, ids, pTaille, tampon, overlapped en pile)
{
    uint32_t count = ARG(7), ids = ARG(8), sizePtr = ARG(9), buffer = ARG(10);
    uint32_t overlapped = Rd32(ctx.r1.u32 + 0x54);
    if (count < 1 || count > 32 || !sizePtr) RET(0x57);   // ERROR_INVALID_PARAMETER
    uint32_t needed = 8;
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t id = Rd32(ids + i * 4), type = (id >> 28) & 0xF;
        needed += 0x28 + ((type == 4 || type == 6) ? (id >> 16) & 0xFFF : 0);
    }
    if (!buffer || Rd32(sizePtr) < needed)
    {
        if (!Rd32(sizePtr)) Wr32(sizePtr, needed);
        RET(0x7A);   // ERROR_INSUFFICIENT_BUFFER
    }
    memset(g_base + buffer, 0, needed);
    Wr32(buffer, count);
    Wr32(buffer + 4, buffer + 8);
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t s = buffer + 8 + i * 0x28;
        Wr32(s + 8, ARG(4));                      // joueur
        Wr32(s + 0x10, Rd32(ids + i * 4));        // identifiant du réglage (from = 0 : non défini)
    }
    KLOG("XamUserReadProfileSettings : %u réglages (non définis)", count);
    if (overlapped) RET(CompleteOverlapped(overlapped, 0, needed));
    RET(0);
}
