// src/game/settings_file.h: settings.txt rewritten by the port without
// losing the lines it doesn't own (keys written by hand for a measurement,
// comments), its own keys replaced where they were; and SettingsSaveDelay,
// one SD card write for a run of taps. Run through
// `scripts/docker.sh test settings`.
#include "game/aim_settings.h"
#include "game/screen_auto.h"
#include "game/settings_file.h"

#include <cstdio>
#include <string>
#include <vector>

using game::merge_settings_text;
using game::SettingLine;

static int g_fail = 0;

#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            g_fail++;                                                    \
        }                                                                \
    } while (0)

#define CHECK_TEXT(got, want)                                                              \
    do {                                                                                   \
        const std::string got_text = (got), want_text = (want);                            \
        if (got_text != want_text) {                                                       \
            std::printf("FAIL %s:%d:\n--- got:\n%s--- want:\n%s---\n", __FILE__, __LINE__, \
                        got_text.c_str(), want_text.c_str());                              \
            g_fail++;                                                                      \
        }                                                                                  \
    } while (0)

// The port's lines as ctr_game.cpp's port_settings_lines() gives them.
static std::vector<SettingLine> port_lines()
{
    return {{"bottom", "map"}, {"mapscale", "2"}, {"port", "core2,limiter"}, {"callret", "1"}};
}

static void test_new_file()
{
    // No file yet: the port's lines alone, in order.
    CHECK_TEXT(merge_settings_text("", port_lines()), "bottom=map\nmapscale=2\nport=core2,limiter\ncallret=1\n");
}

static void test_unknown_keys_kept()
{
    // The measurement keys the port reads but never writes stay, in their places.
    const std::string old_text =
        "bgquantum=20000\n"
        "bottom=info\n"
        "stormrate=512\n"
        "mapscale=1\n"
        "stallms=100\n"
        "aotfns=10-20\n"
        "port=core2\n"
        "callret=0\n";
    CHECK_TEXT(merge_settings_text(old_text, port_lines()),
               "bgquantum=20000\n"
               "bottom=map\n"
               "stormrate=512\n"
               "mapscale=2\n"
               "stallms=100\n"
               "aotfns=10-20\n"
               "port=core2,limiter\n"
               "callret=1\n");
}

static void test_missing_port_keys_appended()
{
    // A hand-written file with only some of the port's keys: the others
    // come at its end, the hand-written lines untouched.
    CHECK_TEXT(merge_settings_text("stormrate=512\nmapscale=4\n", port_lines()),
               "stormrate=512\nmapscale=2\nbottom=map\nport=core2,limiter\ncallret=1\n");
}

static void test_comments_blank_lines_and_no_final_newline()
{
    CHECK_TEXT(merge_settings_text("# my notes\n\nbgslice=0", port_lines()),
               "# my notes\n\nbgslice=0\nbottom=map\nmapscale=2\nport=core2,limiter\ncallret=1\n");
}

static void test_duplicates_dropped()
{
    // A second line of a port key would win at the next start (the file is
    // read top to bottom): it goes, the first is replaced.
    CHECK_TEXT(merge_settings_text("bottom=info\nstallms=9\nbottom=keys\n", port_lines()),
               "bottom=map\nstallms=9\nmapscale=2\nport=core2,limiter\ncallret=1\n");
}

static void test_retired_key_dropped()
{
    // floorqueue= (port='s first form) can't stay after port=: it would
    // override it at the next start.
    CHECK_TEXT(merge_settings_text("port=core2\nfloorqueue=0\nstormdirect=0\n", port_lines(), {"floorqueue"}),
               "port=core2,limiter\nstormdirect=0\nbottom=map\nmapscale=2\ncallret=1\n");
}

static void test_prefix_keys_not_confused()
{
    // "mapscale" isn't "map", nor is "callretx" "callret".
    CHECK_TEXT(merge_settings_text("callretx=5\nmap=1\n", port_lines()),
               "callretx=5\nmap=1\nbottom=map\nmapscale=2\nport=core2,limiter\ncallret=1\n");
}

static void test_crlf_lines_kept()
{
    // A file saved by a Windows editor: its own lines keep their "\r".
    CHECK_TEXT(merge_settings_text("stallms=7\r\n", port_lines()),
               "stallms=7\r\nbottom=map\nmapscale=2\nport=core2,limiter\ncallret=1\n");
}

static void test_saving_twice_changes_nothing()
{
    const std::string once = merge_settings_text("bgquantum=1\nbottom=info\n# x\n", port_lines());
    CHECK_TEXT(merge_settings_text(once, port_lines()), once);
}

static void test_save_delay_one_write_for_a_run_of_taps()
{
    // Taps every 300 ms for 2 s: nothing is due while they go on, one save
    // a second after the last.
    game::SettingsSaveDelay delay(1000, 5000);
    CHECK(!delay.pending());
    CHECK(!delay.due(0));
    int writes = 0;
    for (uint64_t now = 10000; now <= 20000; now += 50) {
        if (now <= 12000 && (now - 10000) % 300 == 0)
            delay.note_change(now);
        if (delay.due(now)) {
            writes++;
            CHECK(now == 12800);   // the last tap at 11800: due a second later
            delay.saved();
        }
    }
    CHECK(writes == 1);
    CHECK(!delay.pending());
}

static void test_save_delay_longest_wait()
{
    // Taps that never stop for a second: saved anyway 5 s after the first.
    game::SettingsSaveDelay delay(1000, 5000);
    uint64_t first_save = 0;
    for (uint64_t now = 0; now <= 8000 && !first_save; now += 100) {
        delay.note_change(now);   // a change every 100 ms
        if (delay.due(now))
            first_save = now;
    }
    CHECK(first_save == 5000);
}

static void test_save_delay_pending_for_a_flush()
{
    // A change not yet due is still pending: the app writes it before the
    // HOME menu or the end.
    game::SettingsSaveDelay delay;
    delay.note_change(100);
    CHECK(delay.pending());
    CHECK(!delay.due(500));
    delay.saved();
    CHECK(!delay.pending());
    CHECK(!delay.due(100000));
}

static void test_aim_lines_written_and_read_back()
{
    // An old settings.txt without the auto-target: its keys are appended
    // (aim=1 first), and what the player edits by hand is read back.
    game::AimSettings aim;
    std::vector<SettingLine> lines = port_lines();
    game::aim_settings_lines(aim, lines);
    const std::string text = merge_settings_text("bottom=info\nmapscale=1\n", lines);
    CHECK(text.find("aim=1\naim_a_mode=enemies\n") != std::string::npos);
    CHECK(text.find("aim_b_range=20\n") != std::string::npos);
    game::AimSettings edited;
    CHECK(game::parse_aim_setting("aim=0", edited) && !edited.enabled);
    CHECK(game::parse_aim_setting("aim_b_mode=corpses", edited) && edited.b.mode == game::AimMode::kCorpses);
    CHECK(game::parse_aim_setting("aim_a_range=999", edited) && edited.a.range_subtiles < 999);   // clamped
    CHECK(!game::parse_aim_setting("aimless=1", edited));
    // Saved again: the edited values, in the same places.
    std::vector<SettingLine> again = port_lines();
    game::aim_settings_lines(edited, again);
    const std::string text2 = merge_settings_text(text, again);
    CHECK(text2.find("aim=0\n") != std::string::npos && text2.find("aim_b_mode=corpses\n") != std::string::npos);
    CHECK(merge_settings_text(text2, again) == text2);
}

// "screenauto=": every switch on by default (as the port always behaved),
// "-name" turns one off, an unknown name or a missing one changes nothing,
// and the names written read back to the same switches.
static void test_screen_auto_line()
{
    using namespace game;
    CHECK(screen_auto_bits("") == kScreenAutoDefault);
    CHECK(kScreenAutoDefault == (kAutoPanel | kAutoNpcMenu | kAutoPanelSide | kAutoKeyboard | kAutoVideo));
    CHECK(screen_auto_bits("-panels") == (kScreenAutoDefault & ~kAutoPanel));
    CHECK(screen_auto_bits("panels,-npcmenu,-video,bogus") == (kScreenAutoDefault & ~(kAutoNpcMenu | kAutoVideo)));
    CHECK(screen_auto_bits("-keyboard\r\n") == (kScreenAutoDefault & ~kAutoKeyboard));
    CHECK_TEXT(screen_auto_names(kScreenAutoDefault), "panels,npcmenu,panelside,keyboard,video");
    const uint32_t some = kAutoPanelSide | kAutoVideo;
    CHECK_TEXT(screen_auto_names(some), "-panels,-npcmenu,panelside,-keyboard,video");
    CHECK(screen_auto_bits(screen_auto_names(some).c_str()) == some);
    CHECK(screen_auto_bits(screen_auto_names(0).c_str()) == 0);
    // The longest line fits load_settings' 64-byte line buffer.
    CHECK(std::string("screenauto=" + screen_auto_names(0) + "\n").size() < 64);
}

int main()
{
    test_new_file();
    test_unknown_keys_kept();
    test_missing_port_keys_appended();
    test_comments_blank_lines_and_no_final_newline();
    test_duplicates_dropped();
    test_retired_key_dropped();
    test_prefix_keys_not_confused();
    test_crlf_lines_kept();
    test_saving_twice_changes_nothing();
    test_save_delay_one_write_for_a_run_of_taps();
    test_save_delay_longest_wait();
    test_save_delay_pending_for_a_flush();
    test_aim_lines_written_and_read_back();
    test_screen_auto_line();
    std::printf("RESULT settings %s (%d failures)\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
}
