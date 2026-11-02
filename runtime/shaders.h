// Shaders traduits hors ligne par XenosRecomp : chargement du cache SPIR-V et association au microcode du jeu
#pragma once
#include <cstdint>
#include <cstddef>
#include <type_traits>
#include <utility>
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
    bool pixel;
    std::vector<uint32_t> spirv;
    // Vertex shader : adresse d'instruction vfetch -> emplacement d'entrée Vulkan (USAGE_LOCATIONS de XenosRecomp)
    std::vector<std::pair<uint32_t, uint32_t>> fetchLocations;
};

// Énumère les instructions de fetch de sommets du microcode (adresse d'instruction, 3 dwords décodés)
template<typename F> void ForEachVertexFetch(const uint8_t* ucode, uint32_t sizeBytes, F&& callback);
void ForEachVertexFetchRaw(const uint8_t* ucode, uint32_t sizeBytes, void (*cb)(void*, uint32_t, const uint32_t*), void* user);
template<typename F> void ForEachVertexFetch(const uint8_t* ucode, uint32_t sizeBytes, F&& callback)
{
    ForEachVertexFetchRaw(ucode, sizeBytes, [](void* u, uint32_t a, const uint32_t* w) { (*static_cast<std::remove_reference_t<F>*>(u))(a, w); }, &callback);
}

void ShadersInit(uint32_t imageBase, uint32_t imageSize);

// Microcode tel que chargé par IM_LOAD (big-endian, en mémoire invitée) -> shader traduit, ou nullptr
const TranslatedShader* ShaderLookup(const uint8_t* ucode, uint32_t sizeBytes, bool pixel);
