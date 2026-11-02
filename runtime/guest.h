// Accès à la mémoire du jeu (big-endian, 4 Go réservés) et helpers pour les imports noyau
#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <atomic>
#include <mutex>
#include <ppc_recomp_shared.h>

extern uint8_t* g_base;

template<typename T> inline T* GuestPtr(uint32_t addr) { return addr ? (T*)(g_base + addr) : nullptr; }
inline uint32_t HostToGuest(const void* p) { return uint32_t((const uint8_t*)p - g_base); }

inline uint8_t  Rd8 (uint32_t a) { return g_base[a]; }
inline uint16_t Rd16(uint32_t a) { uint16_t v; memcpy(&v, g_base + a, 2); return __builtin_bswap16(v); }
inline uint32_t Rd32(uint32_t a) { uint32_t v; memcpy(&v, g_base + a, 4); return __builtin_bswap32(v); }
inline uint64_t Rd64(uint32_t a) { uint64_t v; memcpy(&v, g_base + a, 8); return __builtin_bswap64(v); }
inline void Wr8 (uint32_t a, uint8_t v)  { g_base[a] = v; }
inline void Wr16(uint32_t a, uint16_t v) { v = __builtin_bswap16(v); memcpy(g_base + a, &v, 2); }
inline void Wr32(uint32_t a, uint32_t v) { v = __builtin_bswap32(v); memcpy(g_base + a, &v, 4); }
inline void Wr64(uint32_t a, uint64_t v) { v = __builtin_bswap64(v); memcpy(g_base + a, &v, 8); }

// Allocateurs "bump" par zone (pas de libération réelle pour l'instant)
uint32_t GuestAllocKernel(uint32_t size, uint32_t align = 16);   // structures runtime (PCR, piles, objets)
uint32_t GuestAllocVirtual(uint32_t size);                       // NtAllocateVirtualMemory
uint32_t GuestAllocPhysical(uint32_t size, uint32_t align);      // MmAllocatePhysicalMemoryEx

// Prépare un contexte de thread invité (pile, PCR, KTHREAD) ; renvoie le PCR
void GuestInitThreadContext(PPCContext& ctx, uint32_t stackSize);

// Appelle une fonction invitée par son adresse
inline void GuestCall(PPCContext& ctx, uint32_t addr) { uint8_t* base = g_base; PPC_CALL_INDIRECT_FUNC(addr); }

void LogImport(const char* name, PPCContext& ctx);
#define KLOG(...) do { fprintf(stderr, "[k] " __VA_ARGS__); fputc('\n', stderr); } while (0)
