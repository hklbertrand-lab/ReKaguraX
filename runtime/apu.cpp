// Audio : décodeur XMA matériel (registres 0x7FEA0000, contextes de 64 octets) et sortie XAudio vers SDL3.
//
// Le moteur audio du jeu gère lui-même ses contextes XMA : il les alloue (XMACreateContext) dans le tableau
// dont l'adresse est dans le registre 0x600, puis les "lance" (kick, registre 0x650 + id/32, bit id%32), les
// verrouille (0x690) pendant qu'il les modifie et les remet à zéro (0x6A0). Le matériel lit des paquets
// XMA de 2 Ko dans deux tampons d'entrée et écrit du PCM 16 bits big-endian (entrelacé en stéréo) dans un
// anneau de blocs de 256 octets. Le décodage lui-même est confié au décodeur xma2 de FFmpeg (comme Xenia).
//
// Le mixeur du jeu envoie ensuite chaque trame (256 échantillons x 6 canaux, flottants big-endian, un canal
// après l'autre) via XAudioSubmitRenderDriverFrame ; on la réduit en stéréo pour SDL.
#include "guest.h"
#include <SDL3/SDL.h>
#include <chrono>
#include <thread>
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
}

static constexpr uint32_t kXmaBase = 0x7FEA0000, kContextCount = 320, kPacketBytes = 2048;
enum : uint32_t { REG_CONTEXT_ARRAY = 0x600, REG_CURRENT_CONTEXT = 0x606, REG_KICK = 0x650, REG_LOCK = 0x690, REG_CLEAR = 0x6A0 };

struct XmaContext
{
    bool used = false, enabled = false;
    AVCodecContext* codec = nullptr;
    int channels = 0;
    std::vector<uint8_t> pending;   // PCM décodé pas encore écrit dans l'anneau de sortie
};

static std::mutex s_mutex;
static XmaContext s_contexts[kContextCount];
static uint32_t s_arrayVirtual, s_arrayPhysical;
static AVPacket* s_packet;
static AVFrame* s_frame;
static bool s_log = getenv("IKARUGA_XMALOG");

static uint32_t ContextAddress(uint32_t id) { return 0xA0000000 | (s_arrayPhysical + id * 64); }

// Le jeu lit ses registres par lwbrx, recompilé en lecture mémoire ordinaire : les valeurs doivent être en
// place (little-endian) avant qu'il ne les lise
static void InitArray()
{
    if (s_arrayVirtual) return;
    s_arrayVirtual = GuestAllocPhysical(kContextCount * 64, 0x1000);
    s_arrayPhysical = s_arrayVirtual & 0x1FFFFFFF;
    memset(g_base + s_arrayVirtual, 0, kContextCount * 64);
    Wr32(kXmaBase + REG_CONTEXT_ARRAY * 4, __builtin_bswap32(s_arrayPhysical));
}

// ---------------------------------------------------------------- registres

uint32_t GuestMmioLoad32(uint32_t addr)
{
    if ((addr & 0xFFFF0000) == kXmaBase)
    {
        uint32_t reg = (addr & 0xFFFF) / 4, value = Rd32(addr);
        std::lock_guard l(s_mutex);
        if (reg == REG_CONTEXT_ARRAY) { InitArray(); value = __builtin_bswap32(s_arrayPhysical); }
        return value;
    }
    return Rd32(addr);
}

static void ResetContext(uint32_t id)
{
    XmaContext& c = s_contexts[id];
    c.enabled = false;
    c.pending.clear();
    if (c.codec) avcodec_flush_buffers(c.codec);
    uint32_t a = ContextAddress(id);
    Wr32(a + 0, Rd32(a + 0) & ~((3u << 20) | (0x1Fu << 27)));   // tampons d'entrée invalides, écriture 0
    Wr32(a + 4, Rd32(a + 4) & ~(1u << 31));                      // sortie invalide
    Wr32(a + 36, Rd32(a + 36) & ~0x1Fu);                         // lecture 0
}

void GuestMmioStore32(uint32_t addr, uint32_t value)
{
    Wr32(addr, value);
    if ((addr & 0xFFFF0000) != kXmaBase) return;
    uint32_t reg = (addr & 0xFFFF) / 4, bits = __builtin_bswap32(value);   // registres little-endian (stwbrx)
    std::lock_guard l(s_mutex);
    for (uint32_t i = 0; i < 32; i++)
    {
        if (!(bits & (1u << i))) continue;
        if (s_log) { static int n; if (n++ < 40) KLOG("XMA : registre %03X bit %u", reg, i); }
        if (reg >= REG_KICK && reg < REG_KICK + 10) { uint32_t id = (reg - REG_KICK) * 32 + i; if (id < kContextCount) s_contexts[id].enabled = true; }
        else if (reg >= REG_LOCK && reg < REG_LOCK + 10) { uint32_t id = (reg - REG_LOCK) * 32 + i; if (id < kContextCount) s_contexts[id].enabled = false; }
        else if (reg >= REG_CLEAR && reg < REG_CLEAR + 10) { uint32_t id = (reg - REG_CLEAR) * 32 + i; if (id < kContextCount) ResetContext(id); }
    }
}

uint32_t ApuCreateContext()
{
    std::lock_guard l(s_mutex);
    InitArray();
    for (uint32_t id = 0; id < kContextCount; id++)
        if (!s_contexts[id].used)
        {
            s_contexts[id].used = true;
            memset(g_base + ContextAddress(id), 0, 64);
            if (s_log) KLOG("XMA : contexte %u créé", id);
            return s_arrayVirtual + id * 64;
        }
    return 0;
}

void ApuReleaseContext(uint32_t ptr)
{
    std::lock_guard l(s_mutex);
    uint32_t id = ((ptr & 0x1FFFFFFF) - s_arrayPhysical) / 64;
    if (id >= kContextCount) return;
    ResetContext(id);
    s_contexts[id].used = false;
}

// ---------------------------------------------------------------- décodage

static bool OpenCodec(XmaContext& c, int channels)
{
    if (c.codec && c.channels == channels) return true;
    if (c.codec) avcodec_free_context(&c.codec);
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_XMA2);
    c.codec = avcodec_alloc_context3(codec);
    av_channel_layout_default(&c.codec->ch_layout, channels);
    c.codec->sample_rate = 48000;   // sans effet sur le décodage, la cadence vient du mixeur du jeu
    c.codec->block_align = kPacketBytes;
    // XMA2WAVEFORMATEX réduit : 1 flux, masque de canaux 0 (disposition par défaut)
    c.codec->extradata = (uint8_t*)av_mallocz(34 + AV_INPUT_BUFFER_PADDING_SIZE);
    c.codec->extradata_size = 34;
    c.codec->extradata[0] = 1;
    if (avcodec_open2(c.codec, codec, nullptr) < 0) { KLOG("XMA : ouverture du décodeur impossible"); avcodec_free_context(&c.codec); return false; }
    c.channels = channels;
    return true;
}

// Décode un paquet de 2 Ko ; ajoute le PCM 16 bits big-endian entrelacé à c.pending
static void DecodePacket(XmaContext& c, const uint8_t* data)
{
    s_packet->data = const_cast<uint8_t*>(data);
    s_packet->size = kPacketBytes;
    if (avcodec_send_packet(c.codec, s_packet) < 0) return;
    while (avcodec_receive_frame(c.codec, s_frame) == 0)
    {
        int n = s_frame->nb_samples, ch = c.channels;
        size_t at = c.pending.size();
        c.pending.resize(at + size_t(n) * ch * 2);
        int16_t* out = (int16_t*)(c.pending.data() + at);
        for (int i = 0; i < n; i++)
            for (int k = 0; k < ch; k++)
            {
                float v = ((const float*)s_frame->extended_data[std::min(k, s_frame->ch_layout.nb_channels - 1)])[i];
                int s = int(std::clamp(v, -1.0f, 1.0f) * 32767.0f);
                out[i * ch + k] = int16_t(__builtin_bswap16(uint16_t(int16_t(s))));
            }
        av_frame_unref(s_frame);
    }
}

// Fait avancer un contexte activé : remplit son anneau de sortie tant qu'il y a de la place et des paquets
static void ProcessContext(uint32_t id)
{
    XmaContext& c = s_contexts[id];
    uint32_t a = ContextAddress(id);
    uint32_t d0 = Rd32(a), d1 = Rd32(a + 4), d2 = Rd32(a + 8), d4 = Rd32(a + 16), d9 = Rd32(a + 36);
    if (!(d1 >> 31)) return;   // output_buffer_valid
    uint32_t blockCount = (d0 >> 22) & 0x1F, writeBlock = d0 >> 27, readBlock = d9 & 0x1F;
    if (!blockCount) return;
    int channels = ((d1 >> 29) & 1) ? 2 : 1;
    if (!OpenCodec(c, channels)) { c.enabled = false; return; }
    uint32_t out = 0xA0000000 | (Rd32(a + 28) & 0x1FFFFFFF);
    uint32_t loopCount = (d0 >> 12) & 0xFF;
    static bool warnedLoop;
    if (loopCount && !warnedLoop) { warnedLoop = true; KLOG("XMA : boucles matérielles (loop_count %u) non gérées", loopCount); }

    for (;;)
    {
        // Place libre en blocs (un bloc reste vide : anneau plein et anneau vide ne se confondent pas)
        uint32_t freeBlocks = (readBlock + blockCount - writeBlock - 1) % blockCount;
        if (c.pending.size() >= 256 && freeBlocks)
        {
            uint32_t blocks = std::min<uint32_t>(freeBlocks, uint32_t(c.pending.size() / 256));
            for (uint32_t b = 0; b < blocks; b++)
            {
                memcpy(g_base + out + writeBlock * 256, c.pending.data() + b * 256, 256);
                writeBlock = (writeBlock + 1) % blockCount;
            }
            c.pending.erase(c.pending.begin(), c.pending.begin() + blocks * 256);
            continue;
        }
        if (c.pending.size() >= 256) break;   // sortie pleine : le jeu doit consommer

        // Paquet suivant du tampon d'entrée courant (le décalage de lecture est en bits)
        uint32_t current = d4 >> 31;
        bool valid0 = (d0 >> 20) & 1, valid1 = (d0 >> 21) & 1;
        bool valid = current ? valid1 : valid0;
        if (!valid)
        {
            bool other = current ? valid0 : valid1;
            if (!other) break;   // plus rien à décoder
            current ^= 1;
            d4 = (d4 & 0x7FFFFFFF) | (current << 31);
            d2 = (d2 & ~0x3FFFFFFu) | 32;
            continue;
        }
        uint32_t packets = current ? (d1 & 0xFFF) : (d0 & 0xFFF);
        uint32_t base = Rd32(a + (current ? 24 : 20)) & 0x1FFFFFFF;
        uint32_t index = (d2 & 0x3FFFFFF) / (kPacketBytes * 8);
        if (index >= packets)
        {
            // Tampon épuisé : invalidé, on passe à l'autre
            d0 &= ~(1u << (20 + current));
            current ^= 1;
            d4 = (d4 & 0x7FFFFFFF) | (current << 31);
            d2 = (d2 & ~0x3FFFFFFu) | 32;
            continue;
        }
        DecodePacket(c, g_base + (0xA0000000 | (base + index * kPacketBytes)));
        static uint64_t decoded;
        if (s_log && ++decoded % 200 == 1) KLOG("XMA : %llu paquets décodés (contexte %u, %d canaux, %u paquets dans le tampon %u)", (unsigned long long)decoded, id, channels, packets, current);
        d2 = (d2 & ~0x3FFFFFFu) | ((index + 1) * kPacketBytes * 8 + 32);
    }
    d0 = (d0 & ~(0x1Fu << 27)) | (writeBlock << 27);
    // Le jeu a pu modifier le contexte entre-temps (hors verrou) : on ne réécrit que nos champs
    uint32_t n0 = Rd32(a), n2 = Rd32(a + 8), n4 = Rd32(a + 16);
    Wr32(a, (n0 & ~((0x1Fu << 27) | (3u << 20))) | (d0 & ((0x1Fu << 27) | (3u << 20))));
    Wr32(a + 8, (n2 & ~0x3FFFFFFu) | (d2 & 0x3FFFFFF));
    Wr32(a + 16, (n4 & 0x7FFFFFFF) | (d4 & 0x80000000));
}

static void DecoderThread()
{
    s_packet = av_packet_alloc();
    s_frame = av_frame_alloc();
    for (;;)
    {
        {
            std::lock_guard l(s_mutex);
            for (uint32_t id = 0; id < kContextCount; id++)
                if (s_contexts[id].used && s_contexts[id].enabled) ProcessContext(id);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void ApuInitialize()
{
    { std::lock_guard l(s_mutex); InitArray(); }
    static std::once_flag once;
    std::call_once(once, [] { std::thread(DecoderThread).detach(); });
}

// ---------------------------------------------------------------- sortie XAudio

static SDL_AudioStream* s_stream;

static void OpenOutput()
{
    if (s_stream) return;
    SDL_AudioSpec spec{ SDL_AUDIO_F32, 2, 48000 };
    s_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!s_stream) { KLOG("audio : SDL_OpenAudioDeviceStream a échoué (%s)", SDL_GetError()); return; }
    SDL_ResumeAudioStreamDevice(s_stream);
    KLOG("audio : sortie SDL ouverte (48 kHz stéréo)");
}

// Trame XAudio : 6 canaux (avant G/D, centre, LFE, arrière G/D) de 256 flottants big-endian chacun
void ApuSubmitFrame(uint32_t samples)
{
    OpenOutput();
    if (!s_stream || !samples) return;
    float stereo[256 * 2];
    auto at = [&](int ch, int i) { uint32_t w = Rd32(samples + (ch * 256 + i) * 4); float f; memcpy(&f, &w, 4); return f; };
    for (int i = 0; i < 256; i++)
    {
        float c = at(2, i) * 0.707f;
        stereo[i * 2] = std::clamp(at(0, i) + c + at(4, i) * 0.707f, -1.0f, 1.0f);
        stereo[i * 2 + 1] = std::clamp(at(1, i) + c + at(5, i) * 0.707f, -1.0f, 1.0f);
    }
    SDL_PutAudioStreamData(s_stream, stereo, sizeof(stereo));

    // Débogage : IKARUGA_WAV=fichier -> enregistre la sortie (flottants 48 kHz stéréo, en-tête écrit à la fin)
    static FILE* wav = getenv("IKARUGA_WAV") ? fopen(getenv("IKARUGA_WAV"), "wb") : nullptr;
    static uint32_t wavBytes;
    if (wav)
    {
        if (!wavBytes) fseek(wav, 44, SEEK_SET);
        fwrite(stereo, sizeof(stereo), 1, wav);
        wavBytes += sizeof(stereo);
        uint32_t hdr[11] = { 0x46464952, 36 + wavBytes, 0x45564157, 0x20746D66, 16, 3 | (2 << 16), 48000, 48000 * 8, 8 | (32 << 16), 0x61746164, wavBytes };
        long pos = ftell(wav);
        fseek(wav, 0, SEEK_SET); fwrite(hdr, sizeof(hdr), 1, wav); fseek(wav, pos, SEEK_SET);
        fflush(wav);
    }
}

// Trames encore en attente côté SDL : le thread du pilote XAudio se cale dessus (horloge de la carte son)
bool ApuOutputOpen() { return s_stream != nullptr; }

uint32_t ApuQueuedFrames()
{
    return s_stream ? uint32_t(SDL_GetAudioStreamQueued(s_stream) / (256 * 2 * 4)) : 0;
}
