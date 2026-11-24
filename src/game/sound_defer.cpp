// See sound_defer.h.
#include "game/sound_defer.h"
#include "game/native_sites.h"

#include "runtime/cpu.h"
#include "runtime/rt_host.h"

namespace game {

namespace {

constexpr uint32_t kTable = 0x007bc9a0, kCount = 0x007bc9a4;   // the records and how many
constexpr uint32_t kRecord = 0x8e, kDefer = 0x4d, kStream = 0x4f;
constexpr uint32_t kCountMax = 0x10000;

bool g_on = true;

uint32_t on_table(d2rt::Cpu &c)
{
    const uint32_t count = c.read_u32(kCount);
    const uint32_t table = c.read_u32(kTable);
    if (!g_on || !table || !count || count > kCountMax)
        return count;
    auto *rec = (uint8_t *)c.hostptr(table, count * kRecord);
    if (!rec)
        return count;
    uint32_t set = 0, had = 0, streams = 0;
    for (uint32_t i = 0; i < count; i++, rec += kRecord) {
        if (rec[kStream])
            streams++;
        else if (rec[kDefer])
            had++;
        else {
            rec[kDefer] = 1;
            set++;
        }
    }
    rt::logf("sound: %u of %u sounds read off the game thread at their first play (%u more as sounds.txt has them, %u streams)",
             set, count, had, streams);
    return count;
}

}  // namespace

bool install_sound_defer(d2rt::Cpu &c, bool on)
{
    g_on = on;
    const uint32_t native = rt::native_entry("SoundTable", 0, on_table);
    // native_sites_table.h's sound_defer row, which the AOT recompiler reads
    // too (issue #138).
    return native && apply_sites(c, "sound_defer", {{"SoundTable", native}});
}

}  // namespace game
