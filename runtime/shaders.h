// Shaders traduits hors ligne par XenosRecomp : chargement du cache SPIR-V et association au microcode du jeu
#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>

// Format écrit par XenosRecomp dans build/shaders/shader_cache.cpp
struct ShaderCacheEntry
{
    uint64_t hash;   // XXH3 du conteneur de shader
    uint32_t dxilOffset, dxilSize, spirvOffset, spirvSize, specConstantsMask;
};

struct TranslatedShader
{
    uint64_t containerHash;
    uint32_t specConstantsMask;
    std::vector<uint32_t> spirv;
};

void ShadersInit(uint32_t imageBase, uint32_t imageSize);

// Microcode tel que chargé par IM_LOAD (big-endian, en mémoire invitée) -> shader traduit, ou nullptr
const TranslatedShader* ShaderLookup(const uint8_t* ucode, uint32_t sizeBytes, bool pixel);
