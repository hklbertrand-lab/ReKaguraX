// Accès MMIO du code recompilé (lwbrx/stwbrx vers les registres matériels) : inclus de force avant
// ppc_context.h pour que le bloc XMA (0x7FEA0000) voie les écritures du jeu
#pragma once
#ifdef __cplusplus
#include <cstdint>
uint32_t GuestMmioLoad32(uint32_t addr);
void GuestMmioStore32(uint32_t addr, uint32_t value);
#define PPC_MM_LOAD_U32(x) GuestMmioLoad32(x)
#define PPC_MM_STORE_U32(x, y) GuestMmioStore32(x, y)
#endif
