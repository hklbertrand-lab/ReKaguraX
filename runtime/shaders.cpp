// Association microcode du jeu <-> shaders SPIR-V produits par XenosRecomp.
// D3D patche les instructions vfetch des vertex shaders (format, stride, offset) au chargement :
// on hache donc le microcode en masquant ces instructions, des deux côtés.
#include "shaders.h"
#include "guest.h"
#include <unordered_map>
#include <zstd.h>
#include <smolv.h>
#define XXH_INLINE_ALL
#include <xxhash.h>

extern ShaderCacheEntry g_shaderCacheEntries[];
extern const size_t g_shaderCacheEntryCount;
extern const uint8_t g_compressedSpirvCache[];
extern const size_t g_spirvCacheCompressedSize;
extern const size_t g_spirvCacheDecompressedSize;

static std::vector<uint8_t> s_spirvBlob;
static std::unordered_map<uint64_t, const ShaderCacheEntry*> s_byContainer;
static std::unordered_map<uint64_t, uint64_t> s_byUcode;   // hachage masqué du microcode -> hachage du conteneur
static std::unordered_map<uint64_t, TranslatedShader> s_decoded;
static std::mutex s_mutex;

static uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

// Hachage du microcode en ignorant les instructions de fetch de sommets (patchées par D3D).
// Flot de contrôle Xenos : paires d'instructions sur 3 dwords ; une exec donne adresse, nombre
// d'instructions et 2 bits de séquence par instruction (bit 0 = fetch). Instructions de 3 dwords.
static uint64_t MaskedHash(const uint8_t* ucode, uint32_t sizeBytes)
{
    uint32_t dwords = sizeBytes / 4;
    std::vector<uint32_t> w(dwords);
    for (uint32_t i = 0; i < dwords; i++) w[i] = Be32(ucode + i * 4);

    uint32_t firstInstr = dwords;   // le flot de contrôle s'arrête là où commencent les instructions
    for (uint32_t cf = 0; cf * 3 + 2 < dwords && cf * 3 < firstInstr; cf++)
    {
        uint32_t d0 = w[cf * 3], d1 = w[cf * 3 + 1], d2 = w[cf * 3 + 2];
        uint32_t pair[2][2] = { { d0, d1 & 0xFFFF }, { (d1 >> 16) | (d2 << 16), d2 >> 16 } };
        for (auto& ins : pair)
        {
            uint32_t opcode = (ins[1] >> 12) & 0xF;
            bool exec = (opcode >= 1 && opcode <= 6) || opcode == 13 || opcode == 14;
            if (!exec) continue;
            uint32_t address = ins[0] & 0xFFF, count = (ins[0] >> 12) & 7, sequence = (ins[0] >> 16) & 0xFFF;
            if (count) firstInstr = std::min(firstInstr, address * 3);
            for (uint32_t i = 0; i < count; i++)
            {
                uint32_t at = (address + i) * 3;
                bool fetch = (sequence >> (i * 2)) & 1;
                if (fetch && at + 2 < dwords && (w[at] & 0x1F) == 0)   // vfetch : on masque tout sauf l'opcode
                    w[at] &= 0x1F, w[at + 1] = 0, w[at + 2] = 0;
            }
        }
    }
    return XXH3_64bits(w.data(), w.size() * 4);
}

void ShadersInit(uint32_t imageBase, uint32_t imageSize)
{
    s_spirvBlob.resize(g_spirvCacheDecompressedSize);
    size_t n = ZSTD_decompress(s_spirvBlob.data(), s_spirvBlob.size(), g_compressedSpirvCache, g_spirvCacheCompressedSize);
    if (ZSTD_isError(n)) { KLOG("cache de shaders : échec zstd (%s)", ZSTD_getErrorName(n)); return; }
    for (size_t i = 0; i < g_shaderCacheEntryCount; i++) s_byContainer[g_shaderCacheEntries[i].hash] = &g_shaderCacheEntries[i];

    // Même balayage que XenosRecomp : conteneurs 0x102A11xx dans l'image chargée
    const uint8_t* img = g_base + imageBase;
    uint32_t found = 0, translated = 0;
    for (uint32_t i = 0; i + 36 < imageSize; i += 4)
    {
        const uint8_t* c = img + i;
        uint32_t flags = Be32(c), vs = Be32(c + 4), ps = Be32(c + 8), so = Be32(c + 24);
        if ((flags & 0xFFFFFF00) != 0x102A1100 || uint64_t(vs) + ps > imageSize - i || Be32(c + 28) || Be32(c + 32)) continue;
        uint64_t containerHash = XXH3_64bits(c, size_t(vs) + ps);
        uint32_t po = Be32(c + so), size = Be32(c + so + 4);
        s_byUcode[MaskedHash(c + vs + po, size)] = containerHash;
        found++;
        if (auto it = s_byContainer.find(containerHash); it != s_byContainer.end() && it->second->spirvSize) translated++;
        i += vs + ps - 4;
    }
    KLOG("shaders : %u conteneurs dans l'image, %u traduits en SPIR-V", found, translated);
}

const TranslatedShader* ShaderLookup(const uint8_t* ucode, uint32_t sizeBytes, bool pixel)
{
    uint64_t key = MaskedHash(ucode, sizeBytes);
    std::lock_guard lock(s_mutex);
    if (auto it = s_decoded.find(key); it != s_decoded.end()) return it->second.spirv.empty() ? nullptr : &it->second;

    TranslatedShader& t = s_decoded[key];
    auto u = s_byUcode.find(key);
    if (u == s_byUcode.end()) { KLOG("shader %s inconnu (microcode %u octets)", pixel ? "PS" : "VS", sizeBytes); return nullptr; }
    auto e = s_byContainer.find(u->second);
    if (e == s_byContainer.end() || !e->second->spirvSize) { KLOG("shader %016llx sans SPIR-V", (unsigned long long)u->second); return nullptr; }

    t.containerHash = u->second;
    t.specConstantsMask = e->second->specConstantsMask;
    const uint8_t* smol = s_spirvBlob.data() + e->second->spirvOffset;
    size_t outSize = smolv::GetDecodedBufferSize(smol, e->second->spirvSize);
    t.spirv.resize(outSize / 4);
    if (!smolv::Decode(smol, e->second->spirvSize, t.spirv.data(), outSize)) { t.spirv.clear(); return nullptr; }
    return &t;
}
