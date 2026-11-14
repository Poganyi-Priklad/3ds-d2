// Our natives against cases made from the user's own Game.exe (issue #109):
// tests/d2d/gen_cases.py runs each native's game function under Unicorn and
// records the memory it was given and what it wrote. Here the native runs on
// the same memory, through a test Cpu, and the two are compared:
//   - EAX, where the native's caller reads it;
//   - every byte of Game.exe's writable sections and of the case's window,
//     so a byte the native wrote and the game didn't counts as well.
// A native that hands the call back to the game's code (resume_original) is
// counted as left to the game, not compared.
//
//   native_cases <Game.exe> <cases dir> [Name ...]
//
// The native's own code runs: this file includes d2_native.cpp.
#include "../../src/game/d2_native.cpp"
#include "runtime/bridge.h"

#include <cstdio>
#include <dirent.h>
#include <string>
#include <vector>

namespace game {
namespace {

constexpr uint32_t kImageBase = 0x00400000;
constexpr uint32_t kCaseLo = 0x01000000, kCaseSize = 0x00f00000;   // gen_cases.py's CASE_LO, CASE_SIZE
constexpr uint32_t kStackTop = 0x01ff0000, kArenaEnd = 0x02000000;
constexpr uint32_t kReturnTo = 0x7f00fff0;                          // never run: the native returns in C

// The guest's memory is the arena; registers are plain words.
struct TestCpu : d2rt::Cpu {
    uint32_t regs[d2rt::R_EFLAGS + 1] = {};
    bool in(uint32_t va, uint32_t n) const { return va >= kImageBase && (uint64_t)va + n <= kArenaEnd; }
    bool map(uint32_t, uint32_t, const void *, int) override { return true; }
    bool protect(uint32_t, uint32_t, int) override { return true; }
    bool read(uint32_t va, void *dst, uint32_t n) override
    {
        if (!in(va, n))
            return false;
        std::memcpy(dst, H(va), n);
        return true;
    }
    bool write(uint32_t va, const void *src, uint32_t n) override
    {
        if (!in(va, n))
            return false;
        std::memcpy(H(va), src, n);
        return true;
    }
    void *hostptr(uint32_t va, uint32_t n) override { return in(va, n) ? H(va) : nullptr; }
    uint32_t reg(int r) override { return regs[r]; }
    void set_reg(int r, uint32_t v) override { regs[r] = v; }
    void set_trap(uint32_t, uint32_t, d2rt::TrapFn) override {}
    bool run(uint32_t, const char **fault) override
    {
        if (fault)
            *fault = "no guest code runs in the case test";
        return false;
    }
    uint32_t fault_addr() const override { return 0; }
};

// A scheduler with one thread, the current one: what LoadsPending asks
// (the current thread, for an event's state) and the locks' owner.
struct TestScheduler : d2rt::ThreadScheduler {
    d2rt::GuestThread thread;
    TestScheduler() { thread.id = 1; }
    d2rt::GuestThread *create_thread(uint32_t, uint32_t, uint32_t, bool) override { return nullptr; }
    void resume(d2rt::GuestThread *) override {}
    d2rt::GuestThread *current() override { return &thread; }
    void yield() override {}
    uint32_t wait(d2rt::Waitable *, uint32_t) override { return 0x102; }
    void notify(d2rt::Waitable *) override {}
    void exit_current(uint32_t) override {}
    void run() override {}
    int live_count() override { return 1; }
    void wait_noresult(d2rt::Waitable *, uint32_t) override {}
    uint32_t wait_timeout_result(d2rt::Waitable *, uint32_t, uint32_t r) override { return r; }
    void set_no_preempt(bool) override {}
    void request_shutdown() override {}
    d2rt::GuestThread *thread_by_id(uint32_t id) override { return id == 1 ? &thread : nullptr; }
    std::vector<uint32_t> live_thread_ids() override { return {1}; }
    int live_thread_count() override { return 1; }
    void dump_threads_to(void (*)(const char *)) override {}
    d2rt::GuestThread *set_main(uint32_t, uint32_t, uint32_t) override { return &thread; }
    void set_fault_dispatcher(FaultFn) override {}
    void set_time_sink(std::function<void(uint64_t)>) override {}
    const char *stop_reason() const override { return ""; }
};

// gen_cases.py's EVENT_WAITING and EVENT_SET.
struct TestEvent : d2rt::Waitable {
    bool set;
    explicit TestEvent(bool s) : set(s) {}
    bool try_acquire(d2rt::GuestThread *) override { return set; }
    bool ready(d2rt::GuestThread *) override { return set; }
    const char *kind() const override { return "event"; }
};
constexpr uint32_t kEventWaiting = 0x82000000, kEventSet = 0x82000001;

using NativeFn = uint32_t (*)(Cpu &);
struct Native {
    const char *name;
    NativeFn fn;
};
const Native kNatives[] = {
    {"StormHash", storm_hash},           {"StatGet", stat_get_native},
    {"LightAdd", light_add_native},      {"LightStamp", light_stamp},
    {"LightStampOccluded", light_stamp_occluded},
    {"RoomFind", room_find_native},      {"CollisionCross", collision_cross_native},
    {"UnitWalk", unit_walk},             {"PathSearch", path_search},
    {"AdpcmDecode", adpcm_decode},       {"HuffmanDecode", huffman_decode},
    {"PkExplode", pk_decode},
    {"UnitRelation", unit_relation_native}, {"UnitPickFilter", unit_pick_filter},
    {"SkillLevel", skill_level_native},  {"LangLookup", lang_lookup},
    {"SpriteCacheTouch", cache_touch},   {"SpritePath", sprite_path},
    {"LockedLookup", locked_lookup},     {"CurrentLookup", current_lookup},
    {"CurrentUnlock", current_unlock},   {"LoadsPending", loads_pending},
};

struct Span {
    uint32_t va;
    std::vector<uint8_t> bytes;
};
struct Case {
    uint32_t regs[7];   // eax ecx edx ebx ebp esi edi
    std::vector<uint32_t> args;
    std::vector<Span> before, after;
    uint32_t eax;
};

struct Reader {
    std::vector<uint8_t> data;
    size_t at = 0;
    bool ok = true;
    uint32_t u32()
    {
        if (at + 4 > data.size()) {
            ok = false;
            return 0;
        }
        uint32_t v;
        std::memcpy(&v, &data[at], 4);
        at += 4;
        return v;
    }
    std::vector<uint8_t> bytes(uint32_t n)
    {
        if (at + n > data.size()) {
            ok = false;
            return {};
        }
        std::vector<uint8_t> v(data.begin() + at, data.begin() + at + n);
        at += n;
        return v;
    }
    std::vector<Span> spans()
    {
        std::vector<Span> out(u32());
        for (Span &s : out) {
            s.va = u32();
            s.bytes = bytes(u32());
        }
        return out;
    }
};

bool read_file(const std::string &path, std::vector<uint8_t> &out)
{
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;
    uint8_t buf[65536];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;)
        out.insert(out.end(), buf, buf + n);
    std::fclose(f);
    return true;
}

// Game.exe at its addresses in the arena; the writable sections' extent.
struct Image {
    std::vector<uint8_t> arena, pristine;   // pristine: the arena as loaded
    uint32_t data_lo = 0, data_hi = 0;
};

bool load_image(const char *path, Image &img)
{
    std::vector<uint8_t> file;
    if (!read_file(path, file) || file.size() < 0x400 || file[0] != 'M' || file[1] != 'Z')
        return false;
    auto u16 = [&](size_t o) { return (uint32_t)file[o] | (uint32_t)file[o + 1] << 8; };
    auto u32 = [&](size_t o) { return u16(o) | u16(o + 2) << 16; };
    const uint32_t pe = u32(0x3c), count = u16(pe + 6), opt = u16(pe + 20);
    const uint32_t image = u32(pe + 24 + 56), headers = u32(pe + 24 + 60);
    if (u32(pe + 24 + 28) != kImageBase || kImageBase + image > kCaseLo)
        return false;
    img.arena.assign(kArenaEnd - kImageBase, 0);
    std::memcpy(img.arena.data(), file.data(), headers);
    img.data_lo = kImageBase + image;
    img.data_hi = kImageBase + image;
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t s = pe + 24 + opt + 40 * i;
        const uint32_t vsize = u32(s + 8), rva = u32(s + 12), raw_size = u32(s + 16), raw = u32(s + 20);
        const uint32_t flags = u32(s + 36);
        if (raw + raw_size > file.size() || rva + raw_size > image)
            return false;
        std::memcpy(img.arena.data() + rva, file.data() + raw, raw_size);
        if (flags & 0x80000000u) {   // IMAGE_SCN_MEM_WRITE
            img.data_lo = std::min(img.data_lo, kImageBase + rva);
            img.data_hi = std::max(img.data_hi, kImageBase + rva + std::max(vsize, raw_size));
        }
    }
    img.data_hi = std::min(img.data_hi, kImageBase + image);
    img.pristine.assign(img.arena.begin(), img.arena.begin() + (img.data_hi - kImageBase));
    g_base = img.arena.data() - kImageBase;
    return true;
}

// One native's file: its cases, compared. Returns the number that differ.
int run_file(const std::string &path, Image &img, TestCpu &cpu, int &total)
{
    Reader r;
    if (!read_file(path, r.data) || r.data.size() < 8 || std::memcmp(r.data.data(), "D2NC", 4)) {
        std::printf("%s: not a cases file\n", path.c_str());
        return 1;
    }
    r.at = 4;
    if (r.u32() != 1) {
        std::printf("%s: another version of the cases file\n", path.c_str());
        return 1;
    }
    const std::vector<uint8_t> nb = r.bytes(r.u32());
    const std::string name(nb.begin(), nb.end());
    const uint32_t flags = r.u32(), count = r.u32();
    const Native *native = nullptr;
    for (const Native &n : kNatives)
        if (name == n.name)
            native = &n;
    if (!native) {
        std::printf("%s: no native %s in this runner\n", path.c_str(), name.c_str());
        return 1;
    }
    int bad = 0, left = 0;
    std::vector<uint8_t> expect;
    for (uint32_t i = 0; i < count && r.ok; i++) {
        Case c;
        for (uint32_t &v : c.regs)
            v = r.u32();
        c.args.resize(r.u32());
        for (uint32_t &v : c.args)
            v = r.u32();
        c.before = r.spans();
        c.eax = r.u32();
        c.after = r.spans();
        if (!r.ok)
            break;
        // The memory as the game had it, then what the game left: expected.
        uint32_t win_hi = kCaseLo;
        for (const auto *spans : {&c.before, &c.after})
            for (const Span &s : *spans) {
                if (s.va >= kCaseLo)
                    win_hi = std::max(win_hi, s.va + (uint32_t)s.bytes.size());
            }
        for (const Span &s : c.before)
            std::memcpy(H(s.va), s.bytes.data(), s.bytes.size());
        const uint32_t data_n = img.data_hi - img.data_lo, win_n = win_hi - kCaseLo;
        expect.assign(H(img.data_lo), H(img.data_lo) + data_n);
        expect.insert(expect.end(), H(kCaseLo), H(kCaseLo) + win_n);
        for (const Span &s : c.after) {
            const size_t at = s.va >= kCaseLo ? data_n + (s.va - kCaseLo) : s.va - img.data_lo;
            if (s.va < img.data_lo || at + s.bytes.size() > expect.size()) {
                std::printf("%s case %u: the game wrote %08x, outside what is compared\n", name.c_str(), i, s.va);
                continue;
            }
            std::memcpy(&expect[at], s.bytes.data(), s.bytes.size());
        }
        // The call: the registers, then the arguments above a return address.
        static const int kOrder[7] = {d2rt::R_EAX, d2rt::R_ECX, d2rt::R_EDX, d2rt::R_EBX,
                                      d2rt::R_EBP, d2rt::R_ESI, d2rt::R_EDI};
        for (int k = 0; k < 7; k++)
            cpu.set_reg(kOrder[k], c.regs[k]);
        const uint32_t esp = kStackTop - 4 - 4 * (uint32_t)c.args.size();
        put32(esp, kReturnTo);
        for (size_t k = 0; k < c.args.size(); k++)
            put32(esp + 4 + 4 * (uint32_t)k, c.args[k]);
        cpu.set_reg(d2rt::R_ESP, esp);
        const uint32_t t0 = rt::tick_ms();
        const uint32_t eax = native->fn(cpu);
        // SpriteCacheTouch's stamp: GetTickCount's in the game (the
        // emulator's 0), the slice's clock in the native; taken as equal
        // when the native's is that clock's.
        if (native->fn == cache_touch && c.regs[2]) {
            const uint32_t at = c.regs[2] + 0x10, t = U32(at), t1 = rt::tick_ms();
            const size_t k = at >= kCaseLo ? data_n + (at - kCaseLo) : at - img.data_lo;
            if (t - t0 <= t1 - t0 && k + 4 <= expect.size())
                std::memcpy(H(at), &expect[k], 4);
        }
        if (rt::bridge().take_redirect()) {
            left++;
        } else {
            uint32_t first = 0, diffs = 0;
            for (size_t k = 0; k < expect.size(); k++) {
                const uint32_t va = k < data_n ? img.data_lo + (uint32_t)k : kCaseLo + (uint32_t)(k - data_n);
                if (*H(va) != expect[k] && !diffs++)
                    first = va;
            }
            const bool eax_bad = (flags & 1) && eax != c.eax;
            if (diffs || eax_bad) {
                if (++bad <= 5)
                    std::printf("DIFF %s case %u: %s%u bytes differ (first at %08x: native %02x, game %02x)\n",
                                name.c_str(), i,
                                eax_bad ? (std::string("EAX native ") + std::to_string(eax) + ", game " +
                                           std::to_string(c.eax) + "; ").c_str()
                                        : "",
                                diffs, first, diffs ? *H(first) : 0,
                                diffs ? expect[first >= kCaseLo ? data_n + (first - kCaseLo) : first - img.data_lo]
                                      : 0);
            }
        }
        // Back to the image as loaded and an empty window.
        std::memcpy(H(img.data_lo), img.pristine.data() + (img.data_lo - kImageBase), data_n);
        std::memset(H(kCaseLo), 0, kArenaEnd - kCaseLo);
    }
    if (!r.ok) {
        std::printf("%s: truncated\n", path.c_str());
        return bad + 1;
    }
    total += (int)count;
    std::printf("%-20s %5u cases, %d differ, %d left to the game's code%s\n", name.c_str(), count, bad, left,
                (flags & 1) ? "" : " (EAX not compared: dead after the call)");
    return bad;
}

}  // namespace
}  // namespace game

int main(int argc, char **argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: native_cases <Game.exe> <cases dir> [Name ...]\n");
        return 2;
    }
    static game::Image img;
    if (!game::load_image(argv[1], img)) {
        std::fprintf(stderr, "native_cases: %s is not a Game.exe this test can load\n", argv[1]);
        return 2;
    }
    game::load_native_tables();   // as install_d2_native does, without its hooks
    static game::TestCpu cpu;
    static d2rt::Bridge bridge(&cpu);
    rt::set_bridge_for_tests(&bridge);
    static game::TestScheduler sched;
    wx86_set_scheduler(&sched);
    static game::TestEvent waiting(false), set(true);
    if (wx86_handle_add(&waiting) != game::kEventWaiting || wx86_handle_add(&set) != game::kEventSet) {
        std::fprintf(stderr, "native_cases: WinVita's first handles are not gen_cases.py's events\n");
        return 2;
    }
    std::vector<std::string> names;
    for (int k = 3; k < argc; k++)
        names.push_back(argv[k]);
    if (names.empty()) {
        DIR *d = opendir(argv[2]);
        if (!d) {
            std::fprintf(stderr, "native_cases: no cases in %s\n", argv[2]);
            return 2;
        }
        while (dirent *e = readdir(d)) {
            std::string f = e->d_name;
            if (f.size() > 6 && f.compare(f.size() - 6, 6, ".cases") == 0)
                names.push_back(f.substr(0, f.size() - 6));
        }
        closedir(d);
        std::sort(names.begin(), names.end());
    }
    int bad = 0, total = 0;
    for (const std::string &n : names)
        bad += game::run_file(std::string(argv[2]) + "/" + n + ".cases", img, cpu, total);
    std::printf("RESULT d2d.native_cases %s (%zu natives, %d cases)\n", bad ? "FAIL" : "PASS", names.size(), total);
    return bad ? 1 : 0;
}
