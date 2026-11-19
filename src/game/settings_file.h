// settings.txt (sdmc:/3ds/3ds-d2/settings.txt): how the port rewrites it
// without losing what it doesn't own, and when.
//
// The file is shared: the port writes its own keys (the bottom screen's tab,
// the OPTIONS page's ticks, the dynarec's knobs...), and the player may add
// measurement keys by hand (bgquantum=, stormrate=, stallms=, aotfns=...)
// that the port reads at the start but never writes. Before, every save
// rewrote the file from scratch, so a single tap on the bottom screen threw
// those hand-written lines away. merge_settings_text() keeps them.
//
// And a save used to happen on every tap, on the game's core. SettingsSaveDelay
// waits until the taps stop (one write a second after the last change), but
// never longer than a few seconds after the first unsaved one.
//
// Header only, no platform: the app (src/app/ctr_game.cpp) and the host test
// (tests/settings) use it alike.
#ifndef D2_GAME_SETTINGS_FILE_H
#define D2_GAME_SETTINGS_FILE_H

#include <cstdint>
#include <string>
#include <vector>

namespace game {

// One "key=value" line the port owns and writes.
struct SettingLine {
    std::string key;
    std::string value;
};

// The key of a settings line: the text before its first '=' ("" when the
// line has no '=', such as a blank line or a comment).
inline std::string settings_line_key(const std::string &line)
{
    const size_t equals = line.find('=');
    return equals == std::string::npos ? std::string() : line.substr(0, equals);
}

// settings.txt's new text, from what the file holds now (old_text, "" if it
// doesn't exist) and the port's own lines:
//   - a line of a key the port owns is replaced, where it was, by the
//     port's value; a second line of the same key is dropped (the game
//     reads the file top to bottom, so a stale copy further down would win
//     at the next start);
//   - a line of a retired key (one that a port-owned key replaced, such as
//     floorqueue= by port=) is dropped, so it can't override the new one;
//   - every other line (unknown keys, comments, blank lines) is kept as it
//     was, in its place;
//   - the port's keys that weren't in the file are added at its end, in
//     the order given.
// Lines end in "\n" (a "\r\n" file keeps its "\r" on the kept lines).
inline std::string merge_settings_text(const std::string &old_text, const std::vector<SettingLine> &port_lines,
                                       const std::vector<std::string> &retired_keys = {})
{
    std::vector<bool> already_written(port_lines.size(), false);
    std::string merged;
    size_t line_start = 0;
    while (line_start < old_text.size()) {
        size_t line_end = old_text.find('\n', line_start);
        if (line_end == std::string::npos)
            line_end = old_text.size();
        const std::string line = old_text.substr(line_start, line_end - line_start);
        line_start = line_end + 1;

        std::string key = settings_line_key(line);
        bool is_retired = false;
        for (const std::string &retired : retired_keys)
            if (!key.empty() && key == retired)
                is_retired = true;
        if (is_retired)
            continue;

        bool owned_by_port = false;
        for (size_t i = 0; i < port_lines.size(); i++) {
            if (key.empty() || key != port_lines[i].key)
                continue;
            owned_by_port = true;
            if (!already_written[i]) {
                merged += port_lines[i].key + "=" + port_lines[i].value + "\n";
                already_written[i] = true;
            }
            break;
        }
        if (!owned_by_port)
            merged += line + "\n";
    }
    for (size_t i = 0; i < port_lines.size(); i++)
        if (!already_written[i])
            merged += port_lines[i].key + "=" + port_lines[i].value + "\n";
    return merged;
}

// When to write settings.txt after the bottom screen changed a setting:
// once the changes have stopped for quiet_ms (1 s), or at the latest
// longest_wait_ms (5 s) after the first change not yet saved, so that a run
// of "+"/"-" taps costs one SD card write instead of one per tap. Times in
// milliseconds on any steady clock.
class SettingsSaveDelay {
public:
    explicit SettingsSaveDelay(uint64_t quiet_ms = 1000, uint64_t longest_wait_ms = 5000)
        : quiet_ms_(quiet_ms), longest_wait_ms_(longest_wait_ms) {}

    // A setting changed at now_ms.
    void note_change(uint64_t now_ms)
    {
        if (!pending_)
            first_change_ms_ = now_ms;
        pending_ = true;
        last_change_ms_ = now_ms;
    }

    // Something changed that isn't saved yet.
    bool pending() const { return pending_; }

    // It is time to save: quiet long enough, or waited the longest.
    bool due(uint64_t now_ms) const
    {
        if (!pending_)
            return false;
        return now_ms - last_change_ms_ >= quiet_ms_ || now_ms - first_change_ms_ >= longest_wait_ms_;
    }

    // The file was written: nothing pending.
    void saved() { pending_ = false; }

private:
    uint64_t quiet_ms_, longest_wait_ms_;
    uint64_t first_change_ms_ = 0, last_change_ms_ = 0;
    bool pending_ = false;
};

}  // namespace game

#endif
