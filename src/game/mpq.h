// Finds a file stored in an MPQ archive without compression or encryption,
// as Diablo II stores its cinematics, so it can be read in place: the
// result is the file's byte range within the archive. And reads a small
// file whole (mpq_read_file), as the bottom screen's art is read.
#ifndef D2_GAME_MPQ_H
#define D2_GAME_MPQ_H

#include <cstdint>
#include <string>
#include <vector>

namespace game {

// `name` as the game spells it ("data\\local\\video\\..."); case and slash
// direction do not matter. False if the archive has no such file, or holds
// it compressed or encrypted.
bool mpq_find_stored(const std::string &archive, const std::string &name, uint64_t &offset, uint32_t &size);

// The whole file `name` from `archive`, decrypted and decompressed: stored,
// imploded, or compressed with PKWARE's implode (the only method Diablo
// II's archives use for anything but sounds). False if it is not there, is
// larger than max_size, or uses another method.
bool mpq_read_file(const std::string &archive, const std::string &name, std::vector<uint8_t> &out,
                   uint32_t max_size = 4u << 20);

}  // namespace game

#endif
