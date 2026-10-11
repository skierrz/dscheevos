// dsc-gametest: plays a DS game in the emulator with scripted input and runs
// the DScheevos core against its RAM every frame, with an in-process mock
// RetroAchievements server. Used to check achievement sets end to end.
//
//   dsc-gametest --rom game.nds --set set.json --script inputs.txt \
//                --frames 3000 --expect 101@1200 --never 102
//
// Script lines: "<frame> <KEYS> [hold]" where KEYS is A,B,SELECT,START,
// RIGHT,LEFT,UP,DOWN,R,L,X,Y joined with '+', or "<frame> TOUCH x y [hold]".
// Lines starting with '#' are comments.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <memory>

#include "NDS.h"
#include "Args.h"
#include "NDSCart.h"
#include "SPI_Firmware.h"
#include "Platform.h"

extern "C" {
#include "dscheevos.h"
#include "rc_runtime.h"
}

using namespace melonDS;

// ---- hooks the shared runner platform.cpp expects ----
static bool g_stopped = false, g_verbose = false;
void hl_signal_stop() { g_stopped = true; }
int hl_net_send(u8*, int len) { return len; }
int hl_net_recv(u8*) { return 0; }
void hl_log(Platform::LogLevel level, const char* msg)
{
    if (g_verbose || level == Platform::LogLevel::Error) std::fprintf(stderr, "[emu] %s", msg);
}

// ---- emulated game memory, RetroAchievements DS address space ----
static NDS* g_nds;
static uint32_t read_memory(uint32_t a, uint8_t* buf, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
    {
        uint32_t x = a + i;
        if (x < 0x400000) buf[i] = g_nds->MainRAM[x & g_nds->MainRAMMask];
        else if (x >= 0x1000000 && x < 0x1004000) buf[i] = g_nds->ARM9.DTCM[x - 0x1000000];
        else return i;
    }
    return n;
}

static uint32_t g_frame;
static uint32_t millis() { return (uint32_t)((uint64_t)g_frame * 1000 / 60); }

// ---- mock server ----
static std::string g_set_json;
struct Award { int id; bool hardcore; uint32_t frame; };
static std::vector<Award> g_awards;
static std::vector<std::pair<int, int>> g_lb_submits;  // leaderboard, score
static bool g_session_hardcore = false;

static std::string param(const std::string& post, const std::string& key)
{
    size_t p = 0;
    while (p < post.size())
    {
        size_t e = post.find('&', p);
        if (e == std::string::npos) e = post.size();
        std::string kv = post.substr(p, e - p);
        if (kv.rfind(key + "=", 0) == 0) return kv.substr(key.size() + 1);
        p = e + 1;
    }
    return "";
}

static void http_send(const dsc_http_request_t* req)
{
    std::string post = req->post_data ? req->post_data : "";
    std::string r = param(post, "r"), body = "{\"Success\":true}";
    if (r == "login2") body = "{\"Success\":true,\"User\":\"tester\",\"Token\":\"tok\",\"Score\":0,\"Messages\":0}";
    else if (r == "achievementsets") body = g_set_json;
    else if (r == "startsession") { g_session_hardcore = param(post, "h") == "1"; body = "{\"Success\":true,\"Unlocks\":[],\"HardcoreUnlocks\":[]}"; }
    else if (r == "awardachievement")
    {
        int id = std::atoi(param(post, "a").c_str());
        // the frame being evaluated, which trails g_frame when idle is held back (--idle-every)
        dsc_stats_t st;
        dsc_get_stats(&st);
        g_awards.push_back({id, param(post, "h") == "1", g_frame + 1 - (st.frames_snapshotted - st.frames_evaluated)});
        body = "{\"Success\":true,\"Score\":1,\"SoftcoreScore\":0,\"AchievementID\":" + std::to_string(id) + ",\"AchievementsRemaining\":1}";
    }
    else if (r == "submitlbentry")
    {
        int id = std::atoi(param(post, "i").c_str()), score = std::atoi(param(post, "s").c_str());
        g_lb_submits.push_back({id, score});
        body = "{\"Success\":true,\"Response\":{\"Score\":" + std::to_string(score) + ",\"BestScore\":" + std::to_string(score) +
               ",\"RankInfo\":{\"Rank\":1,\"NumEntries\":1},\"TopEntries\":[]}}";
    }
    dsc_http_done(req->handle, 200, body.c_str(), body.size());
}

static void on_event(const dsc_event_t* e)
{
    switch (e->type)
    {
    case DSC_EVENT_GAME_LOADED: std::printf("frame %u: game loaded: %s\n", g_frame, e->title ? e->title : "?"); break;
    case DSC_EVENT_GAME_LOAD_FAILED: std::printf("frame %u: game load FAILED: %s\n", g_frame, e->detail ? e->detail : ""); break;
    case DSC_EVENT_UNLOCK: std::printf("frame %u: UNLOCK %u \"%s\" (%u pts)\n", g_frame, e->id, e->title ? e->title : "", e->points); break;
    case DSC_EVENT_DEGRADED: std::printf("frame %u: DEGRADED (%u frames dropped)\n", g_frame, e->id); break;
    default: break;
    }
}
static void on_log(const char* m) { if (g_verbose) std::fprintf(stderr, "[rc] %s\n", m); }

// ---- reference: rcheevos runtime reading live RAM at the end of each frame ----
// In this harness DScheevos snapshots at the same moment, so both must agree.
static std::map<int, uint32_t> g_ref_unlocks;
static uint32_t ref_peek(uint32_t address, uint8_t* buffer, uint32_t num_bytes, void*)
{
    return read_memory(address, buffer, num_bytes);
}
static void ref_event(const rc_runtime_event_t* e)
{
    if (e->type == RC_RUNTIME_EVENT_ACHIEVEMENT_TRIGGERED && !g_ref_unlocks.count((int)e->id))
        g_ref_unlocks[(int)e->id] = g_frame;
}
static std::vector<std::pair<int, std::string>> parse_set_achievements(const std::string& json)
{
    // tiny extraction of "ID" and "MemAddr" pairs; test sets are generated by our own script
    std::vector<std::pair<int, std::string>> out;
    size_t p = 0;
    while ((p = json.find("\"ID\":", p)) != std::string::npos)
    {
        int id = std::atoi(json.c_str() + p + 5);
        size_t m = json.find("\"MemAddr\":", p);
        size_t next = json.find("\"ID\":", p + 5);
        if (m == std::string::npos || (next != std::string::npos && m > next)) { p += 5; continue; }
        size_t q1 = json.find('"', m + 10), q2 = json.find('"', q1 + 1);
        out.push_back({id, json.substr(q1 + 1, q2 - q1 - 1)});
        p = q2;
    }
    return out;
}

// ---- input script ----
struct Input { uint32_t frame, hold; u32 keys; bool touch; int x, y; };
static u32 key_bits(const std::string& s)
{
    static const std::map<std::string, u32> names = {
        {"A", 1 << 0}, {"B", 1 << 1}, {"SELECT", 1 << 2}, {"START", 1 << 3}, {"RIGHT", 1 << 4}, {"LEFT", 1 << 5},
        {"UP", 1 << 6}, {"DOWN", 1 << 7}, {"R", 1 << 8}, {"L", 1 << 9}, {"X", 1 << 10}, {"Y", 1 << 11}};
    u32 bits = 0;
    std::stringstream ss(s);
    std::string k;
    while (std::getline(ss, k, '+'))
    {
        auto it = names.find(k);
        if (it == names.end()) { std::fprintf(stderr, "unknown key %s\n", k.c_str()); std::exit(2); }
        bits |= it->second;
    }
    return bits;
}
static std::vector<Input> load_script(const std::string& path)
{
    std::vector<Input> v;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line))
    {
        if (line.empty() || line[0] == '#') continue;
        std::stringstream ss(line);
        Input in{};
        std::string what;
        ss >> in.frame >> what;
        if (what == "TOUCH") { in.touch = true; ss >> in.x >> in.y; }
        else in.keys = key_bits(what);
        if (!(ss >> in.hold)) in.hold = 2;
        v.push_back(in);
    }
    return v;
}

static std::string read_text(const std::string& p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) { std::fprintf(stderr, "cannot read %s\n", p.c_str()); std::exit(2); }
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

int main(int argc, char** argv)
{
    std::string rom, set, script, dumpdir, shot;
    uint32_t frames = 3600, idle_every = 1;
    bool hardcore = true;
    struct Expect { int id; uint32_t from, to; };
    std::vector<Expect> expect;  // id unlocked within [from, to]
    std::vector<int> never;
    std::vector<std::pair<int, int>> expect_lb;  // id, score
    std::vector<uint32_t> dumps;
    for (int i = 1; i < argc; i++)
    {
        std::string a = argv[i];
        auto next = [&]() -> std::string { if (i + 1 >= argc) std::exit(2); return argv[++i]; };
        if (a == "--rom") rom = next();
        else if (a == "--set") set = next();
        else if (a == "--script") script = next();
        else if (a == "--frames") frames = std::stoul(next());
        else if (a == "--softcore") hardcore = false;
        else if (a == "--expect")
        {
            // ID, ID@TO or ID@FROM-TO
            std::string e = next();
            size_t at = e.find('@');
            Expect x{std::stoi(e.substr(0, at)), 0, UINT32_MAX};
            if (at != std::string::npos)
            {
                std::string r = e.substr(at + 1);
                size_t dash = r.find('-');
                if (dash == std::string::npos) x.to = std::stoul(r);
                else { x.from = std::stoul(r.substr(0, dash)); x.to = std::stoul(r.substr(dash + 1)); }
            }
            expect.push_back(x);
        }
        else if (a == "--never") never.push_back(std::stoi(next()));
        else if (a == "--expect-lb") { std::string e = next(); size_t eq = e.find('='); expect_lb.push_back({std::stoi(e.substr(0, eq)), std::stoi(e.substr(eq + 1))}); }
        else if (a == "--dump") { std::stringstream ss(next()); std::string t; while (std::getline(ss, t, ',')) dumps.push_back(std::stoul(t)); }
        else if (a == "--dump-dir") dumpdir = next();
        else if (a == "--screenshot") shot = next();
        else if (a == "--verbose") g_verbose = true;
        else if (a == "--idle-every") idle_every = std::stoul(next());
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    if (rom.empty()) { std::fprintf(stderr, "usage: dsc-gametest --rom X.nds [--set S.json] [--script I.txt] [--frames N] [--expect ID[@[FROM-]TO]] [--expect-lb ID=SCORE] [--never ID] [--dump F1,F2 --dump-dir D] [--softcore] [--idle-every N]\n"); return 2; }

    NDSArgs args;
    args.JIT = std::nullopt;
    args.Firmware = Firmware(0);
    auto ndsp = std::make_unique<NDS>(std::move(args));  // several MB: keep it off the stack
    NDS& nds = *ndsp;
    g_nds = &nds;
    auto data = read_text(rom);
    auto cart = NDSCart::ParseROM((const u8*)data.data(), (u32)data.size(), nullptr, std::nullopt);
    if (!cart) { std::fprintf(stderr, "ROM did not parse\n"); return 2; }
    nds.Reset();
    nds.SetNDSCart(std::move(cart));
    nds.SetupDirectBoot(std::filesystem::path(rom).filename().string());
    nds.Start();

    char hash[33] = {0};
    if (!set.empty())
    {
        g_set_json = read_text(set);
        // the mock serves the set for any hash; the real hash is checked by the hash tests
        std::snprintf(hash, sizeof hash, "%s", "00000000000000000000000000000000");
        dsc_platform_t p = {read_memory, millis, http_send, on_event, on_log};
        dsc_config_t c = {"http://mock", "tester", "tok", hardcore ? 1 : 0, 8192, idle_every > 1 ? 1024u : 0u, 0};
        if (dsc_init(&p, &c) != 0) { std::fprintf(stderr, "dsc_init failed\n"); return 2; }
        dsc_begin_session(hash);
        dsc_idle();  // the mock answers at once: login, set and session are done before frame 0
    }
    rc_runtime_t ref;
    rc_runtime_init(&ref);
    if (!set.empty())
        for (auto& [id, mem] : parse_set_achievements(g_set_json))
            if (rc_runtime_activate_achievement(&ref, (uint32_t)id, mem.c_str(), nullptr, 0) != RC_OK)
                std::printf("reference: could not parse achievement %d\n", id);

    auto inputs = script.empty() ? std::vector<Input>{} : load_script(script);
    for (g_frame = 0; g_frame < frames && !g_stopped; g_frame++)
    {
        u32 held = 0;
        bool touching = false;
        int tx = 0, ty = 0;
        for (auto& in : inputs)
            if (g_frame >= in.frame && g_frame < in.frame + in.hold)
            {
                if (in.touch) { touching = true; tx = in.x; ty = in.y; }
                else held |= in.keys;
            }
        nds.SetKeyMask(~held & 0xFFF);
        if (touching) nds.TouchScreen(tx, ty); else nds.ReleaseScreen();

        nds.RunFrame();

        if (!set.empty())
        {
            dsc_vblank();
            // --idle-every N: idle time only every Nth frame, so frames queue up as in a long load
            if ((g_frame + 1) % idle_every == 0 || g_frame + 1 == frames)
                dsc_idle();
            rc_runtime_do_frame(&ref, [](const rc_runtime_event_t* e) { ref_event(e); }, ref_peek, nullptr, nullptr);
        }
        for (auto d : dumps)
            if (d == g_frame)
            {
                std::string p = (dumpdir.empty() ? std::string(".") : dumpdir) + "/ram_" + std::to_string(d) + ".bin";
                FILE* f = std::fopen(p.c_str(), "wb");
                std::fwrite(nds.MainRAM, 1, 0x400000, f);
                std::fwrite(nds.ARM9.DTCM, 1, 0x4000, f);
                std::fclose(f);
            }
    }

    if (!shot.empty())
    {
        void *top, *bot;
        if (nds.GPU.GetFramebuffers(&top, &bot))
        {
            FILE* f = std::fopen(shot.c_str(), "wb");
            std::fprintf(f, "P6\n256 384\n255\n");
            for (const u32* fb : {(const u32*)top, (const u32*)bot})
                for (int i = 0; i < 256 * 192; i++) { u8 rgb[3] = {u8(fb[i] >> 16), u8(fb[i] >> 8), u8(fb[i])}; std::fwrite(rgb, 1, 3, f); }
            std::fclose(f);
        }
    }

    int fails = 0;
    if (!set.empty())
    {
        dsc_stats_t s;
        dsc_get_stats(&s);
        std::printf("stats: %u frames evaluated, %u dropped, %u addresses, %u live reads, session %s\n",
                    s.frames_evaluated, s.frames_dropped, s.addresses, s.live_reads, g_session_hardcore ? "hardcore" : "softcore");
        for (auto& e : expect)
        {
            const Award* got = nullptr;
            for (auto& a : g_awards) if (a.id == e.id) got = &a;
            if (!got) { std::printf("FAIL: %d never unlocked\n", e.id); fails++; }
            else if (got->frame < e.from || got->frame > e.to) { std::printf("FAIL: %d unlocked at frame %u, expected %u-%u\n", e.id, got->frame, e.from, e.to); fails++; }
            else if (hardcore && !got->hardcore) { std::printf("FAIL: %d sent as softcore\n", e.id); fails++; }
            else std::printf("ok: %d unlocked at frame %u%s\n", e.id, got->frame, got->hardcore ? " (hardcore)" : "");
        }
        for (int id : never)
        {
            bool hit = false;
            for (auto& a : g_awards) if (a.id == id) hit = true;
            if (hit) { std::printf("FAIL: %d unlocked but must not\n", id); fails++; }
            else std::printf("ok: %d not unlocked\n", id);
        }
        for (auto& [id, score] : expect_lb)
        {
            bool hit = false;
            for (auto& [i, sc] : g_lb_submits) if (i == id && sc == score) hit = true;
            if (hit) std::printf("ok: leaderboard %d submitted %d\n", id, score);
            else { std::printf("FAIL: leaderboard %d never submitted %d\n", id, score); fails++; }
        }
        if (s.frames_dropped) { std::printf("FAIL: %u frames dropped\n", s.frames_dropped); fails++; }
        // DScheevos must match the reference frame-for-frame
        std::map<int, uint32_t> dsc;
        for (auto& a : g_awards) if (!dsc.count(a.id)) dsc[a.id] = a.frame;
        for (auto& [id, f] : g_ref_unlocks)
            if (!dsc.count(id)) { std::printf("FAIL: reference unlocked %d at frame %u, DScheevos did not\n", id, f); fails++; }
            else if (dsc[id] != f) { std::printf("FAIL: %d at frame %u, reference frame %u\n", id, dsc[id], f); fails++; }
        for (auto& [id, f] : dsc)
            if (!g_ref_unlocks.count(id)) { std::printf("FAIL: DScheevos unlocked %d at frame %u, reference did not\n", id, f); fails++; }
        std::printf("reference check: %zu unlocks compared\n", g_ref_unlocks.size());
        dsc_shutdown();
    }
    rc_runtime_destroy(&ref);
    std::printf("%s\n", fails ? "RESULT FAIL" : "RESULT PASS");
    return fails ? 1 : 0;
}
