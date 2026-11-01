// Réimplémentation des fonctions noyau Xbox 360 (xboxkrnl / xam) utilisées par Ikaruga
#include "guest.h"
#include <SDL3/SDL.h>
#include <chrono>
#include <condition_variable>
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
static std::shared_ptr<KObject> FromDispatcher(uint32_t addr)
{
    std::lock_guard l(s_objMutex);
    if (auto it = s_dispatchers.find(addr); it != s_dispatchers.end()) return it->second;
    std::shared_ptr<KObject> obj;
    uint8_t type = Rd8(addr);
    int32_t state = (int32_t)Rd32(addr + 4);
    if (type == 0 || type == 1)
    {
        auto ev = std::make_shared<KEvent>(); ev->manualReset = type == 0; ev->signaled = state != 0; obj = ev;
    }
    else if (type == 5)
    {
        auto sem = std::make_shared<KSemaphore>(); sem->count = state; sem->max = (int32_t)Rd32(addr + 0x10); obj = sem;
    }
    else
    {
        KLOG("objet dispatcher de type %u inconnu à %08X", type, addr);
        obj = std::make_shared<KEvent>();
    }
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
KFUNC(NtSetEvent) { if (auto o = FromHandle(ARG(3))) o->Signal(); RET(STATUS_SUCCESS); }
KFUNC(NtClearEvent) { if (auto e = std::dynamic_pointer_cast<KEvent>(FromHandle(ARG(3)))) e->Reset(); RET(STATUS_SUCCESS); }
KFUNC(KeSetEvent) { FromDispatcher(ARG(3))->Signal(); RET(0); }
KFUNC(KeResetEvent) { if (auto e = std::dynamic_pointer_cast<KEvent>(FromDispatcher(ARG(3)))) e->Reset(); RET(0); }

KFUNC(KeInitializeSemaphore)   // (sem, count, limit)
{
    Wr8(ARG(3), 5); Wr32(ARG(3) + 4, ARG(4)); Wr32(ARG(3) + 0x10, ARG(5));
    std::lock_guard l(s_objMutex);
    s_dispatchers.erase(ARG(3));
}
KFUNC(KeReleaseSemaphore)   // (sem, increment, adjustment, wait)
{
    auto sem = std::dynamic_pointer_cast<KSemaphore>(FromDispatcher(ARG(3)));
    RET(sem ? sem->Release(ARG(5)) : 0);
}

KFUNC(KeWaitForSingleObject)   // (object, reason, mode, alertable, pTimeout)
{
    RET(FromDispatcher(ARG(3))->Wait(TimeoutMs(ARG(7))) ? STATUS_SUCCESS : STATUS_TIMEOUT);
}
KFUNC(NtWaitForSingleObjectEx)   // (handle, mode, alertable, pTimeout)
{
    auto o = FromHandle(ARG(3));
    if (!o) RET(STATUS_INVALID_HANDLE);
    RET(o->Wait(TimeoutMs(ARG(6))) ? STATUS_SUCCESS : STATUS_TIMEOUT);
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

static void ThreadMain(std::shared_ptr<KThread> t)
{
    {
        std::unique_lock l(t->suspendMutex);
        t->suspendCv.wait(l, [&] { return !t->suspended; });
    }
    PPCContext ctx;
    GuestInitThreadContext(ctx, t->stackSize);
    t->guestThread = Rd32(ctx.r13.u32 + 0x100);
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
    uint32_t h = NewHandle(t);
    if (ARG(3)) Wr32(ARG(3), h);
    if (ARG(5)) Wr32(ARG(5), h);
    std::thread(ThreadMain, t).detach();
    // Attendre que le KTHREAD invité existe pour que ObReferenceObjectByHandle le renvoie
    if (!t->suspended) while (!t->guestThread) std::this_thread::yield();
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

KFUNC(XamInputGetCapabilities)   // (user, flags, caps)
{
    if (!Pad(ARG(3))) RET(1167);   // ERROR_DEVICE_NOT_CONNECTED
    memset(g_base + ARG(5), 0, 0x14);
    Wr8(ARG(5), 1); Wr8(ARG(5) + 1, 1);   // XINPUT_DEVTYPE_GAMEPAD / SUBTYPE_GAMEPAD
    RET(0);
}

KFUNC(XamInputGetState)   // (user, flags, state)
{
    SDL_Gamepad* p = Pad(ARG(3));
    if (!p) RET(1167);
    static std::atomic<uint32_t> packet;
    uint32_t s = ARG(5);
    uint16_t b = 0;
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
KFUNC(XNotifyGetNext) { RET(0); }
KFUNC(XamNotifyCreateListener) { RET(NewHandle(std::make_shared<KEvent>())); }
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
    for (uint32_t i = 0; i < 64; i++) Wr32(ARG(3) + i * 4, 0x80000000);   // paquets type 2 (NOP)
    GpuOnSwap();
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
