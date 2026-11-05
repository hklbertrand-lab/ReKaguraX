// ReKaguraX : assistant d'installation communautaire.
// Aucune donnée ni aucun code du jeu n'est distribué : l'utilisateur fournit son propre paquet LIVE d'Ikaruga
// (copié depuis sa Xbox 360), l'assistant le vérifie, l'extrait, recompile le code PowerPC et les shaders du jeu
// avec les outils fournis (XenonRecomp, XenosRecomp, clang), puis compile ikaruga(.exe) sur la machine.
//
// Disposition du kit (à côté de rkx-setup) :
//   bin/        XenonRecomp, XenosRecomp (+ dxcompiler), xex_dump, ninja
//   toolchain/  clang + lld + bibliothèques (llvm-mingw sous Windows, LLVM sous Linux)
//   src/        runtime, game/, config/, en-têtes et bibliothèques tierces précompilées (lib/)
// Résultat : ikaruga(.exe) et extracted/ à côté de rkx-setup ; work/ (code généré) est supprimé à la fin.
#include <SDL3/SDL.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

#ifdef _WIN32
static const char* kExe = ".exe";
#else
static const char* kExe = "";
#endif

// Empreintes SHA-1 de default.xex acceptées (Ikaruga XBLA, Title ID 58410878)
static const char* kKnownXex[] = { "38357af38b21db2997dcffcd1ac5d78e7b48291d" };

// ---------------------------------------------------------------------------------------------------------------------
// Journal affiché dans la fenêtre
static std::mutex s_logMutex;
static std::vector<std::string> s_log;
static std::atomic<int> s_progress{ -1 };   // 0..1000, -1 = indéterminé
static std::atomic<int> s_state{ 0 };        // 0 attente du fichier, 1 en cours, 2 terminé, 3 échec
static std::string s_status = "Choisissez votre paquet LIVE d'Ikaruga";

static void Log(const std::string& line)
{
    std::lock_guard l(s_logMutex);
    s_log.push_back(line);
    if (s_log.size() > 2000) s_log.erase(s_log.begin(), s_log.begin() + 1000);
    fprintf(stderr, "%s\n", line.c_str());
}

static void Status(const std::string& s)
{
    { std::lock_guard l(s_logMutex); s_status = s; }
    Log("== " + s);
}

// ---------------------------------------------------------------------------------------------------------------------
// SHA-1
static std::string Sha1File(const fs::path& p)
{
    uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    auto block = [&](const uint8_t* b) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) w[i] = b[i * 4] << 24 | b[i * 4 + 1] << 16 | b[i * 4 + 2] << 8 | b[i * 4 + 3];
        for (int i = 16; i < 80; i++) { uint32_t v = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16]; w[i] = v << 1 | v >> 31; }
        uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++)
        {
            uint32_t f, k;
            if (i < 20) { f = (bb & c) | (~bb & d); k = 0x5A827999; }
            else if (i < 40) { f = bb ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (bb & c) | (bb & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = bb ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t t = (a << 5 | a >> 27) + f + e + k + w[i];
            e = d; d = c; c = bb << 30 | bb >> 2; bb = a; a = t;
        }
        h[0] += a; h[1] += bb; h[2] += c; h[3] += d; h[4] += e;
    };
    std::ifstream f(p, std::ios::binary);
    if (!f) return "";
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), {});
    uint64_t bits = uint64_t(data.size()) * 8;
    data.push_back(0x80);
    while (data.size() % 64 != 56) data.push_back(0);
    for (int i = 7; i >= 0; i--) data.push_back(uint8_t(bits >> (i * 8)));
    for (size_t i = 0; i < data.size(); i += 64) block(&data[i]);
    char out[41];
    for (int i = 0; i < 5; i++) snprintf(out + i * 8, 9, "%08x", h[i]);
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// Extraction STFS (paquets LIVE/PIRS/CON) : portage de tools/stfs_extract.py
struct Stfs
{
    std::ifstream f;
    uint32_t base = 0, shift = 0, ftCount = 0, ftBlock = 0;
    std::string magic;

    static uint32_t I24le(const uint8_t* b) { return b[0] | b[1] << 8 | b[2] << 16; }
    static uint32_t I24be(const uint8_t* b) { return b[0] << 16 | b[1] << 8 | b[2]; }

    std::vector<uint8_t> Read(uint64_t off, size_t n)
    {
        std::vector<uint8_t> v(n);
        f.clear(); f.seekg(off); f.read((char*)v.data(), n);
        v.resize(f.gcount());
        return v;
    }
    bool Open(const fs::path& p)
    {
        f.open(p, std::ios::binary);
        if (!f) return false;
        auto h = Read(0, 0x400);
        if (h.size() < 0x400) return false;
        magic.assign((char*)h.data(), 4);
        if (magic != "LIVE" && magic != "PIRS" && magic != "CON ") return false;
        uint32_t hdrSize = h[0x340] << 24 | h[0x341] << 16 | h[0x342] << 8 | h[0x343];
        base = (hdrSize + 0xFFF) & 0xF000;
        shift = (h[0x37B] & 1) ? 0 : 1;
        ftCount = h[0x37C] | h[0x37D] << 8;
        ftBlock = I24le(&h[0x37E]);
        return true;
    }
    uint64_t DataBlock(uint64_t b)
    {
        uint64_t r = (((b + 0xAA) / 0xAA) << shift) + b;
        if (b < 0xAA) return r;
        if (b < 0x70E4) return r + (((b + 0x70E4) / 0x70E4) << shift);
        return (1ull << shift) + r + (((b + 0x70E4) / 0x70E4) << shift);
    }
    uint64_t BlockOff(uint64_t b) { return base + (DataBlock(b) << 12); }
    uint64_t HashOff(uint64_t b)
    {
        uint64_t n = (b / 0xAA) * 0xAB;
        n += ((b / 0x70E4) + 1) << shift;
        if (b / 0x70E4) n += 1ull << shift;
        if (b < 0xAA) n = 0;
        return base + (n << 12) + (b % 0xAA) * 0x18;
    }
    uint32_t NextBlock(uint32_t b) { auto v = Read(HashOff(b) + 0x15, 3); return v.size() == 3 ? I24be(v.data()) : 0xFFFFFF; }

    struct Entry { std::string name; bool dir, cons; uint32_t blocks, start, size; int parent, idx; };
    std::vector<Entry> Entries()
    {
        std::vector<uint8_t> raw;
        uint32_t b = ftBlock;
        for (uint32_t i = 0; i < ftCount; i++) { auto v = Read(BlockOff(b), 0x1000); raw.insert(raw.end(), v.begin(), v.end()); b = NextBlock(b); }
        std::vector<Entry> out;
        for (size_t i = 0; i + 0x40 <= raw.size(); i += 0x40)
        {
            const uint8_t* e = &raw[i];
            uint8_t fl = e[0x28];
            if (!(fl & 0x3F)) continue;
            out.push_back({ std::string((const char*)e, fl & 0x3F), bool(fl & 0x80), bool(fl & 0x40), I24le(e + 0x29), I24le(e + 0x2F),
                uint32_t(e[0x34] << 24 | e[0x35] << 16 | e[0x36] << 8 | e[0x37]), int16_t(e[0x32] << 8 | e[0x33]), int(i / 0x40) });
        }
        return out;
    }
    bool Extract(const fs::path& outDir)
    {
        auto ents = Entries();
        if (ents.empty()) return false;
        auto pathOf = [&](const Entry& e) {
            std::vector<std::string> parts{ e.name };
            const Entry* cur = &e;
            for (int guard = 0; cur->parent != -1 && guard < 64; guard++)
            {
                const Entry* p = nullptr;
                for (auto& x : ents) if (x.idx == cur->parent) p = &x;
                if (!p) break;
                parts.insert(parts.begin(), p->name); cur = p;
            }
            fs::path r = outDir;
            for (auto& s : parts) r /= s;
            return r;
        };
        for (auto& e : ents)
        {
            fs::path p = pathOf(e);
            if (e.dir) { fs::create_directories(p); continue; }
            fs::create_directories(p.parent_path());
            std::ofstream o(p, std::ios::binary);
            int64_t left = e.size;
            uint32_t b = e.start;
            for (uint32_t i = 0; i < e.blocks && left > 0; i++)
            {
                auto v = Read(BlockOff(b), size_t(std::min<int64_t>(0x1000, left)));
                o.write((const char*)v.data(), v.size());
                left -= 0x1000;
                b = e.cons ? b + 1 : NextBlock(b);
            }
            Log("  " + p.lexically_relative(outDir).generic_string());
        }
        return true;
    }
};

// ---------------------------------------------------------------------------------------------------------------------
// Lance un outil du kit et recopie sa sortie dans le journal ; onLine peut lire la progression
static bool Run(const std::vector<std::string>& args, const fs::path& cwd, void (*onLine)(const std::string&) = nullptr)
{
    std::vector<const char*> argv;
    std::string shown;
    for (auto& a : args) { argv.push_back(a.c_str()); shown += a + " "; }
    argv.push_back(nullptr);
    Log("$ " + shown);

    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
    SDL_SetStringProperty(props, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, cwd.string().c_str());
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
    SDL_Process* proc = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (!proc) { Log(std::string("impossible de lancer : ") + SDL_GetError()); return false; }

    SDL_IOStream* out = SDL_GetProcessOutput(proc);
    std::string line;
    char buf[4096];
    for (;;)
    {
        size_t n = SDL_ReadIO(out, buf, sizeof(buf));
        if (n == 0 && SDL_GetIOStatus(out) != SDL_IO_STATUS_NOT_READY) break;
        if (n == 0) { SDL_Delay(5); continue; }
        for (size_t i = 0; i < n; i++)
        {
            if (buf[i] == '\n' || buf[i] == '\r') { if (!line.empty()) { if (onLine) onLine(line); Log(line); } line.clear(); }
            else line += buf[i];
        }
    }
    if (!line.empty()) Log(line);
    int code = -1;
    SDL_WaitProcess(proc, true, &code);
    SDL_DestroyProcess(proc);
    if (code != 0) Log("échec (code " + std::to_string(code) + ")");
    return code == 0;
}

// Progression de ninja : "[12/105] ..."
static void NinjaProgress(const std::string& line)
{
    int a, b;
    if (sscanf(line.c_str(), "[%d/%d]", &a, &b) == 2 && b > 0) s_progress = 300 + 700 * a / b;
}

// La police de SDL_RenderDebugText n'a que l'ASCII : on retire les accents pour l'affichage
static std::string Ascii(const std::string& s)
{
    static const struct { const char* u; char a; } map[] = { {"é",'e'},{"è",'e'},{"ê",'e'},{"ë",'e'},{"à",'a'},{"â",'a'},{"ç",'c'},
        {"î",'i'},{"ï",'i'},{"ô",'o'},{"ù",'u'},{"û",'u'},{"É",'E'},{"È",'E'},{"À",'A'} };
    std::string o;
    for (size_t i = 0; i < s.size();)
    {
        bool hit = false;
        for (auto& m : map) if (!s.compare(i, strlen(m.u), m.u)) { o += m.a; i += strlen(m.u); hit = true; break; }
        if (hit) continue;
        unsigned char c = s[i];
        if (c < 0x80) o += char(c), i++;
        else { o += '?'; i++; while (i < s.size() && (s[i] & 0xC0) == 0x80) i++; }
    }
    return o;
}

static std::string Fwd(const fs::path& p) { return p.generic_string(); }

// Chemin dans une ligne "build" de ninja : ":", " " et "$" s'échappent avec "$"
static std::string NinjaPath(const fs::path& p)
{
    std::string o;
    for (char c : Fwd(p)) { if (c == ':' || c == ' ' || c == '$') o += '$'; o += c; }
    return o;
}

// ---------------------------------------------------------------------------------------------------------------------
// build.ninja : mêmes options que le CMakeLists d'Ikaruga
static bool WriteNinja(const fs::path& kit, const fs::path& work, const fs::path& outExe)
{
    fs::path src = kit / "src", tc = kit / "toolchain" / "bin";
    std::string inc;
    for (auto d : { work / "ppc", src / "runtime", src / "include", src / "include" / "simde", src / "include" / "XenonUtils", src / "include" / "ffmpeg" })
        inc += " -I" + Fwd(d);
    std::string common = "-DVK_NO_PROTOTYPES -O3 -DNDEBUG -march=x86-64-v3 -Wno-unused-label -Wno-unused-variable" + inc;

    std::ofstream n(work / "build.ninja");
    n << "cxx = " << Fwd(tc / (std::string("clang++") + kExe)) << "\n";
    n << "cc = " << Fwd(tc / (std::string("clang") + kExe)) << "\n";
    n << "cxxflags = -std=gnu++20 -include cstdlib -include cstring " << common << "\n";
    n << "cflags = " << common << "\n";
    n << "rule cxx\n  command = $cxx $cxxflags $extra -c $in -o $out\n  description = $in\n";
    n << "rule cc\n  command = $cc $cflags -c $in -o $out\n  description = $in\n";
#ifdef _WIN32
    n << "rule link\n  command = $cxx $in -o $out " << Fwd(src / "lib" / "libXenonUtils.a") << " " << Fwd(src / "lib" / "libdisasm.a") << " "
      << Fwd(src / "lib" / "libzstd.a") << " " << Fwd(src / "lib" / "libavcodec.a") << " " << Fwd(src / "lib" / "libavutil.a") << " "
      << Fwd(src / "lib" / "libSDL3.dll.a") << " -lbcrypt -lwinmm -mwindows\n  description = édition des liens\n";
#else
    n << "rule link\n  command = $cxx $in -o $out " << Fwd(src / "lib" / "libXenonUtils.a") << " " << Fwd(src / "lib" / "libdisasm.a") << " "
      << Fwd(src / "lib" / "libzstd.a") << " " << Fwd(src / "lib" / "libavcodec.a") << " " << Fwd(src / "lib" / "libavutil.a") << " "
      << "-L" << Fwd(src / "lib") << " -lSDL3 -Wl,-rpath,'$$ORIGIN' -pthread -lm -ldl\n  description = édition des liens\n";
#endif
    std::vector<std::string> objs;
    auto add = [&](const fs::path& f, const char* rule, const std::string& extra = "") {
        std::string obj = NinjaPath(work / "obj" / (f.parent_path().filename().string() + "_" + f.filename().string() + ".o"));
        objs.push_back(obj);
        n << "build " << obj << ": " << rule << " " << NinjaPath(f) << "\n";
        if (!extra.empty()) n << "  extra = " << extra << "\n";
    };
    for (auto& e : fs::directory_iterator(work / "ppc"))
        if (e.path().extension() == ".cpp") add(e.path(), "cxx", "-w -include " + Fwd(src / "runtime" / "mmio.h"));
    for (auto dir : { src / "runtime", src / "game" })
        for (auto& e : fs::directory_iterator(dir))
            if (e.path().extension() == ".cpp") add(e.path(), "cxx");
    add(work / "shaders" / "shader_cache.cpp", "cxx");
    add(src / "include" / "smolv.cpp", "cxx");
    add(src / "include" / "volk.c", "cc");
    n << "build " << NinjaPath(outExe) << ": link";
    for (auto& o : objs) n << " " << o;
    n << "\n";
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
static fs::path s_kit;
static std::string s_package;

static bool Install()
{
    fs::path kit = s_kit, bin = kit / "bin", work = kit / "work", extracted = kit / "extracted";
    std::error_code ec;
    fs::remove_all(work, ec);
    fs::create_directories(work / "ppc");
    fs::create_directories(work / "shaders" / "in");
    fs::create_directories(work / "obj");

    // 1) Paquet -> extracted/
    Status("Extraction du paquet");
    s_progress = 0;
    Stfs st;
    if (!st.Open(s_package)) { Status("Ce fichier n'est pas un paquet LIVE/PIRS/CON de Xbox 360"); return false; }
    fs::remove_all(extracted, ec);
    if (!st.Extract(extracted)) { Status("Paquet illisible"); return false; }

    // 2) Vérification
    Status("Vérification de default.xex");
    std::string sha = Sha1File(extracted / "default.xex");
    bool known = false;
    for (auto k : kKnownXex) known |= sha == k;
    if (!known)
    {
        Log("SHA-1 : " + (sha.empty() ? std::string("default.xex absent") : sha));
        Status("Ce paquet n'est pas une version d'Ikaruga prise en charge");
        fs::remove_all(extracted, ec);
        return false;
    }
    s_progress = 50;

    // 3) Code PowerPC -> C++ (XenonRecomp). La config pointe vers les fichiers de ce dossier.
    Status("Recompilation du code du jeu");
    {
        std::ifstream in(kit / "src" / "config" / "ikaruga.toml");
        std::string toml((std::istreambuf_iterator<char>(in)), {});
        auto repl = [&](const std::string& a, const std::string& b) { auto p = toml.find(a); if (p != std::string::npos) toml.replace(p, a.size(), b); };
        // XenonRecomp colle ces chemins au dossier de la config : chemins relatifs à work/ (../extracted reste valable)
        repl("\"../ppc\"", "\"ppc\"");
        std::ofstream(work / "ikaruga.toml") << toml;
        fs::copy_file(kit / "src" / "config" / "ikaruga_switch_tables.toml", work / "ikaruga_switch_tables.toml", fs::copy_options::overwrite_existing);
    }
    if (!Run({ Fwd(bin / (std::string("XenonRecomp") + kExe)), Fwd(work / "ikaruga.toml"), Fwd(kit / "src" / "include" / "ppc_context.h") }, work))
    { Status("XenonRecomp a échoué"); return false; }
    s_progress = 150;

    // 4) Shaders : image du XEX -> HLSL -> SPIR-V (XenosRecomp)
    Status("Traduction des shaders");
    if (!Run({ Fwd(bin / (std::string("xex_dump") + kExe)), Fwd(extracted / "default.xex"), Fwd(work / "shaders" / "in" / "default.bin") }, work))
    { Status("xex_dump a échoué"); return false; }
    if (!Run({ Fwd(bin / (std::string("XenosRecomp") + kExe)), Fwd(work / "shaders" / "in"), Fwd(work / "shaders" / "shader_cache.cpp"),
               Fwd(kit / "src" / "include" / "shader_common.h") }, bin))
    { Status("XenosRecomp a échoué"); return false; }
    s_progress = 300;

    // 5) Compilation
    Status("Compilation (plusieurs minutes)");
    fs::path exe = kit / (std::string("ikaruga") + kExe);
    WriteNinja(kit, work, exe);
    unsigned jobs = std::max(1u, std::thread::hardware_concurrency());
    if (!Run({ Fwd(bin / (std::string("ninja") + kExe)), "-j", std::to_string(jobs) }, work, NinjaProgress))
    { Status("La compilation a échoué"); return false; }

    fs::remove_all(work, ec);
    s_progress = 1000;
    Status("Installation terminée : lancez Ikaruga");
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
static SDL_Window* s_window;

static void OnFile(void*, const char* const* files, int)
{
    if (!files || !files[0]) return;   // annulé : on reste sur l'écran d'accueil
    s_package = files[0];
    s_state = 1;
    std::thread([] { s_state = Install() ? 2 : 3; }).detach();
}

static void AskFile()
{
    static const SDL_DialogFileFilter filters[] = { { "Paquet Xbox 360 (LIVE)", "*" } };
    SDL_ShowOpenFileDialog(OnFile, nullptr, s_window, filters, 1, nullptr, false);
}

static void LaunchGame()
{
    fs::path exe = s_kit / (std::string("ikaruga") + kExe);
    std::string e = exe.string();
    const char* argv[] = { e.c_str(), nullptr };
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void*)argv);
    SDL_SetStringProperty(props, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, s_kit.string().c_str());
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
    SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
}

int main(int argc, char** argv)
{
    s_kit = fs::path(SDL_GetBasePath());
#ifndef _WIN32
    // ld.lld du kit a besoin de libxml2/ICU (toolchain/ldlibs) et XenosRecomp de libdxcompiler.so (bin/)
    {
        std::string lp = Fwd(s_kit / "toolchain" / "ldlibs") + ":" + Fwd(s_kit / "bin");
        if (const char* old = getenv("LD_LIBRARY_PATH"); old && *old) lp += std::string(":") + old;
        setenv("LD_LIBRARY_PATH", lp.c_str(), 1);
        SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "LD_LIBRARY_PATH", lp.c_str(), true);
    }
#endif
    // Mode sans fenêtre (tests, scripts) : rkx-setup --headless <paquet>
    if (argc > 2 && !strcmp(argv[1], "--headless")) { s_package = argv[2]; return Install() ? 0 : 1; }
    if (!SDL_Init(SDL_INIT_VIDEO)) return 1;
    SDL_Renderer* ren = nullptr;
    if (!SDL_CreateWindowAndRenderer("ReKaguraX - installation", 960, 600, SDL_WINDOW_RESIZABLE, &s_window, &ren)) return 1;
    SDL_SetRenderVSync(ren, 1);

    if (argc > 1) { const char* files[] = { argv[1], nullptr }; OnFile(nullptr, files, 0); }   // paquet en argument (tests)
    else
    {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "ReKaguraX",
            "ReKaguraX ne contient aucune donnée du jeu.\n\n"
            "Choisissez le paquet LIVE d'Ikaruga que vous avez acheté et copié depuis votre propre Xbox 360 "
            "(fichier sans extension, dossier Content/.../58410878/000D0000/).\n\n"
            "Le jeu sera vérifié, extrait et recompilé sur cet ordinateur (quelques minutes).", s_window);
        AskFile();
    }

    bool running = true;
    while (running)
    {
        SDL_Event ev;
        while (SDL_PollEvent(&ev))
        {
            if (ev.type == SDL_EVENT_QUIT && s_state != 1) running = false;
            if (ev.type == SDL_EVENT_KEY_DOWN || ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN || ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)
            {
                if (s_state == 0 || s_state == 3) { s_state = 0; AskFile(); }
                else if (s_state == 2) { LaunchGame(); running = false; }
            }
        }

        int w, h;
        SDL_GetRenderOutputSize(ren, &w, &h);
        SDL_SetRenderScale(ren, 2, 2);
        SDL_SetRenderDrawColor(ren, 16, 18, 24, 255);
        SDL_RenderClear(ren);
        std::string status;
        std::vector<std::string> tail;
        {
            std::lock_guard l(s_logMutex);
            status = s_status;
            int lines = (h / 2 - 60) / 10;
            for (size_t i = s_log.size() > size_t(lines) ? s_log.size() - lines : 0; i < s_log.size(); i++) tail.push_back(s_log[i]);
        }
        SDL_SetRenderDrawColor(ren, 240, 240, 240, 255);
        SDL_RenderDebugText(ren, 8, 8, Ascii(status).c_str());
        const char* hint = s_state == 0 ? "Cliquez ou appuyez sur une touche pour choisir le fichier"
                         : s_state == 2 ? "Cliquez ou appuyez sur une touche pour lancer le jeu"
                         : s_state == 3 ? "Échec : cliquez pour choisir un autre fichier" : "";
        SDL_RenderDebugText(ren, 8, 20, Ascii(hint).c_str());
        if (int p = s_progress; p >= 0)
        {
            SDL_FRect bg{ 8, 34, float(w / 2 - 16), 8 }, fg{ 8, 34, float((w / 2 - 16) * p / 1000), 8 };
            SDL_SetRenderDrawColor(ren, 60, 60, 70, 255); SDL_RenderFillRect(ren, &bg);
            SDL_SetRenderDrawColor(ren, 90, 170, 255, 255); SDL_RenderFillRect(ren, &fg);
        }
        SDL_SetRenderScale(ren, 1, 1);
        SDL_SetRenderDrawColor(ren, 150, 160, 170, 255);
        float y = 100;
        for (auto& l : tail) { SDL_RenderDebugText(ren, 16, y, Ascii(l).substr(0, (w - 32) / 8).c_str()); y += 10; }
        SDL_RenderPresent(ren);
    }
    SDL_Quit();
    return 0;
}
