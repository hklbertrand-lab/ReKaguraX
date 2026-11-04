// Interruptions livrées sur le CPU visé. Sur Xbox 360, une interruption destinée au CPU n interrompt le
// thread qui y tourne et s'exécute dans son contexte : D3D en dépend (le thread principal "participe" ainsi
// au traitement des commandes sans jamais écrire dans le ring en même temps qu'un autre CPU).
// Ici, l'émetteur (thread GPU, vblank) dépose l'interruption dans la file du CPU ; un thread invité de ce CPU
// l'exécute à un point sûr (db16cyc, attente noyau, réactivation des interruptions). Faute de preneur
// rapide, l'émetteur l'exécute lui-même (comportement d'avant).
#include "guest.h"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <thread>
#include <vector>

void KernelWakeWaiters();

struct PendingInterrupt
{
    uint32_t callback, source, arg;
    bool taken = false, done = false;
};

static std::mutex s_mutex;
static std::condition_variable s_doneCv;
static std::deque<PendingInterrupt*> s_queues[8];
static std::atomic<uint32_t> s_pending[8];
static std::vector<PPCContext*> s_threads;   // threads invités pouvant recevoir des interruptions
static thread_local PPCContext* t_ctx;
static thread_local bool t_inInterrupt;

static uint32_t CpuOf(const PPCContext& ctx) { return Rd8(ctx.r13.u32 + 0x10C) & 7; }

void InterruptsRegisterThread(PPCContext& ctx)
{
    std::lock_guard l(s_mutex);
    t_ctx = &ctx;
    s_threads.push_back(&ctx);
}

PPCContext* InterruptsCurrentContext() { return t_ctx; }
uint32_t InterruptsCurrentCpu() { return t_ctx ? CpuOf(*t_ctx) : 99; }

// db16cyc (indice "boucle d'attente" du PowerPC) : les attentes actives du jeu cèdent régulièrement le cœur ;
// sur une machine à 4 cœurs, elles préemptaient sinon le thread GPU ou celui qui fait le vrai travail.
// Une attente qui dure plus de 0,5 ms (le thread principal qui attend l'image suivante) passe en sommeil :
// un yield seul gardait un cœur à 100 % (17 W sur Steam Deck au lieu de ~10)
static void SpinHint()
{
    static thread_local uint32_t spins;
    static thread_local std::chrono::steady_clock::time_point last, streak;
    if ((++spins & 255) != 0) return;
    auto now = std::chrono::steady_clock::now();
    if (now - last > std::chrono::microseconds(200)) streak = now;   // du vrai travail depuis la dernière fois
    if (now - streak > std::chrono::microseconds(500))
    {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        last = std::chrono::steady_clock::now();
        return;
    }
    last = now;
    std::this_thread::yield();
}

void GuestPollInterrupts(PPCContext& ctx)
{
    SpinHint();
    if (t_inInterrupt || !(ctx.msr & 0x8000)) return;
    uint32_t cpu = CpuOf(ctx);
    if (!s_pending[cpu].load(std::memory_order_relaxed)) return;
    PendingInterrupt* p;
    {
        std::lock_guard l(s_mutex);
        if (s_queues[cpu].empty()) return;
        p = s_queues[cpu].front();
        s_queues[cpu].pop_front();
        s_pending[cpu]--;
        p->taken = true;
    }
    // Gestionnaire sur une copie du contexte, sous la pile courante, interruptions masquées
    PPCContext ictx = ctx;
    ictx.r1.u64 = (ctx.r1.u32 - 0x4000) & ~0xFu;
    ictx.msr = 0;
    ictx.r3.u64 = p->source;
    ictx.r4.u64 = p->arg;
    t_inInterrupt = true;
    GuestCall(ictx, p->callback);
    t_inInterrupt = false;
    {
        std::lock_guard l(s_mutex);
        p->done = true;
    }
    s_doneCv.notify_all();
}

// Point de contrôle depuis le noyau (attentes) : utilise le contexte du thread courant
void InterruptsPollCurrent()
{
    if (t_ctx) GuestPollInterrupts(*t_ctx);
}

// Livre une interruption au CPU ; renvoie false si l'appelant doit l'exécuter lui-même
static std::atomic<uint32_t> s_delivered[8], s_fallback[8], s_noThread[8];

static void LogStats()
{
    static auto last = std::chrono::steady_clock::now();
    if (!getenv("IKARUGA_IRQLOG") || std::chrono::steady_clock::now() - last < std::chrono::seconds(3)) return;
    last = std::chrono::steady_clock::now();
    for (int c = 0; c < 6; c++)
        if (s_delivered[c] || s_fallback[c] || s_noThread[c])
            KLOG("interruptions CPU%d : livrées %u, repli %u, sans thread %u", c, s_delivered[c].load(), s_fallback[c].load(), s_noThread[c].load());
}

bool InterruptsDeliver(uint32_t cpu, uint32_t callback, uint32_t source, uint32_t arg)
{
    cpu &= 7;
    LogStats();
    PendingInterrupt p{ callback, source, arg };
    std::unique_lock l(s_mutex);
    bool hasThread = false;
    for (PPCContext* c : s_threads) if (CpuOf(*c) == cpu) hasThread = true;
    if (!hasThread) { s_noThread[cpu]++; return false; }
    s_queues[cpu].push_back(&p);
    s_pending[cpu]++;
    l.unlock();
    KernelWakeWaiters();
    l.lock();
    // 2 ms au plus : au-delà l'émetteur (vblank, GPU) exécute lui-même le gestionnaire. Avec 20 ms, un thread
    // du CPU visé occupé ailleurs décalait toute la cadence de vblank (images de 30 à 75 ms en fin de partie)
    if (!s_doneCv.wait_for(l, std::chrono::milliseconds(2), [&] { return p.taken; }))
    {
        // Personne n'a pris l'interruption à temps : on la retire et l'appelant l'exécute
        auto& q = s_queues[cpu];
        for (auto it = q.begin(); it != q.end(); ++it) if (*it == &p) { q.erase(it); s_pending[cpu]--; break; }
        s_fallback[cpu]++;
        return false;
    }
    s_doneCv.wait(l, [&] { return p.done; });
    s_delivered[cpu]++;
    return true;
}
