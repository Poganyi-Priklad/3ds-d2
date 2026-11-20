// MPQ lookup (see mpq.h). The archive's hash table maps a name, through
// three hashes of it, to an entry of its block table: position, sizes and
// flags. Both tables are encrypted with keys derived from fixed names. A
// compressed file is a run of sectors (512 << the header's shift bytes
// each, the last shorter) behind a table of their offsets; an encrypted one
// has both encrypted with a key from its name.
#include "game/mpq.h"

#include "game/pklib_explode.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <vector>

namespace game {
namespace {

uint32_t g_crypt[0x500];
bool g_crypt_ready = false;

void init_crypt()
{
    uint32_t seed = 0x00100001;
    for (int i = 0; i < 0x100; i++)
        for (int j = i; j < 0x500; j += 0x100) {
            seed = (seed * 125 + 3) % 0x2AAAAB;
            const uint32_t hi = (seed & 0xFFFF) << 16;
            seed = (seed * 125 + 3) % 0x2AAAAB;
            g_crypt[j] = hi | (seed & 0xFFFF);
        }
    g_crypt_ready = true;
}

// kind 0: hash table index, 1 and 2: the two check values, 3: key.
uint32_t hash(const std::string &s, int kind)
{
    uint32_t seed1 = 0x7FED7FED, seed2 = 0xEEEEEEEE;
    for (unsigned char ch : s) {
        ch = ch == '/' ? '\\' : (unsigned char)std::toupper(ch);
        seed1 = g_crypt[(kind << 8) + ch] ^ (seed1 + seed2);
        seed2 = ch + seed1 + seed2 + (seed2 << 5) + 3;
    }
    return seed1;
}

void decrypt(uint32_t *words, size_t n, uint32_t key)
{
    uint32_t seed = 0xEEEEEEEE;
    for (size_t i = 0; i < n; i++) {
        seed += g_crypt[0x400 + (key & 0xFF)];
        const uint32_t v = words[i] ^ (key + seed);
        key = ((~key << 0x15) + 0x11111111) | (key >> 0x0B);
        seed = v + seed + (seed << 5) + 3;
        words[i] = v;
    }
}

bool read_table(FILE *f, uint64_t pos, uint32_t entries, const char *key_name, std::vector<uint32_t> &out)
{
    out.resize((size_t)entries * 4);
    if (std::fseek(f, (long)pos, SEEK_SET) != 0 || std::fread(out.data(), 16, entries, f) != entries)
        return false;
    decrypt(out.data(), out.size(), hash(key_name, 3));
    return true;
}

// The block table entry of `name` (position, packed size, size, flags) and
// the archive's sector shift; the file is left open at no particular place.
bool find_block(FILE *f, const std::string &name, uint32_t blk_out[4], uint32_t &sector_shift)
{
    if (!g_crypt_ready)
        init_crypt();
    uint32_t h[8];   // magic, header size, archive size, version | sector shift, hash pos, block pos, counts
    if (std::fread(h, 4, 8, f) != 8 || h[0] != 0x1A51504D /* "MPQ\x1A" */ || !h[6] || (h[6] & (h[6] - 1)) != 0)
        return false;
    std::vector<uint32_t> hashes, blocks;
    if (!read_table(f, h[4], h[6], "(hash table)", hashes) || !read_table(f, h[5], h[7], "(block table)", blocks))
        return false;
    sector_shift = h[3] >> 16;
    const uint32_t n = h[6], a = hash(name, 1), b = hash(name, 2);
    for (uint32_t i = hash(name, 0) & (n - 1), tries = 0; tries < n; i = (i + 1) & (n - 1), tries++) {
        const uint32_t *e = &hashes[(size_t)i * 4];   // check a, check b, locale | platform, block
        if (e[3] == 0xFFFFFFFF)
            return false;   // end of the chain
        if (e[0] != a || e[1] != b || e[3] >= h[7])
            continue;
        std::memcpy(blk_out, &blocks[(size_t)e[3] * 4], 16);
        return (blk_out[3] & 0x80000000u) != 0;
    }
    return false;
}

constexpr uint32_t kImploded = 0x100, kCompressed = 0x200, kEncrypted = 0x10000, kFixKey = 0x20000,
                   kSingleUnit = 0x01000000;
constexpr uint8_t kMaskPkware = 0x08;

}  // namespace

bool mpq_find_stored(const std::string &archive, const std::string &name, uint64_t &offset, uint32_t &size)
{
    FILE *f = std::fopen(archive.c_str(), "rb");
    if (!f)
        return false;
    bool found = false;
    uint32_t blk[4], shift = 0;
    // Exists, and neither imploded, compressed nor encrypted.
    if (find_block(f, name, blk, shift) && !(blk[3] & (kImploded | kCompressed | kEncrypted)) && blk[1] == blk[2]) {
        offset = blk[0];
        size = blk[2];
        found = true;
    }
    std::fclose(f);
    return found;
}

bool mpq_read_file(const std::string &archive, const std::string &name, std::vector<uint8_t> &out, uint32_t max_size)
{
    out.clear();
    FILE *f = std::fopen(archive.c_str(), "rb");
    if (!f)
        return false;
    uint32_t blk[4], shift = 0;
    bool ok = find_block(f, name, blk, shift);
    const uint32_t pos = ok ? blk[0] : 0, packed = ok ? blk[1] : 0, size = ok ? blk[2] : 0, flags = ok ? blk[3] : 0;
    ok = ok && size <= max_size && !(flags & kSingleUnit) && shift < 16;
    // The key: the hash of the name without its folders (adjusted by the
    // place and size for "fix key" files).
    uint32_t key = 0;
    if (ok && (flags & kEncrypted)) {
        const size_t slash = name.find_last_of("\\/");
        key = hash(slash == std::string::npos ? name : name.substr(slash + 1), 3);
        if (flags & kFixKey)
            key = (key + pos) ^ size;
    }
    const uint32_t sector = 512u << shift, count = (size + sector - 1) / sector;
    std::vector<uint32_t> offsets(count + 1);
    if (ok && (flags & (kImploded | kCompressed))) {
        // A table of where each sector starts, and where the last ends.
        ok = std::fseek(f, (long)pos, SEEK_SET) == 0 && std::fread(offsets.data(), 4, count + 1, f) == count + 1;
        if (ok && (flags & kEncrypted))
            decrypt(offsets.data(), offsets.size(), key - 1);
    } else if (ok) {
        for (uint32_t i = 0; i <= count; i++)
            offsets[i] = i < count ? i * sector : size;
    }
    out.resize(ok ? size : 0);
    std::vector<uint8_t> raw;
    for (uint32_t i = 0; ok && i < count; i++) {
        const uint32_t want = i + 1 < count ? sector : size - i * sector;
        ok = offsets[i] <= offsets[i + 1] && offsets[i + 1] <= packed + 4 * (count + 1) && offsets[i + 1] - offsets[i] <= sector + 64;
        const uint32_t len = ok ? offsets[i + 1] - offsets[i] : 0;
        raw.assign(((size_t)len + 3) & ~(size_t)3, 0);
        ok = ok && std::fseek(f, (long)(pos + offsets[i]), SEEK_SET) == 0 && std::fread(raw.data(), 1, len, f) == len;
        if (!ok)
            break;
        if (flags & kEncrypted)
            decrypt(reinterpret_cast<uint32_t *>(raw.data()), len / 4, key + i);
        uint8_t *dst = out.data() + (size_t)i * sector;
        if (len == want) {
            std::memcpy(dst, raw.data(), want);   // stored as it is
        } else if (flags & kImploded) {
            ok = pk_explode_alone(raw.data(), len, dst, want) == want;
        } else if ((flags & kCompressed) && len > 1 && raw[0] == kMaskPkware) {
            ok = pk_explode_alone(raw.data() + 1, len - 1, dst, want) == want;
        } else {
            ok = false;   // another method (Huffman, ADPCM, zlib...): not for art
        }
    }
    std::fclose(f);
    if (!ok)
        out.clear();
    return ok;
}

}  // namespace game
