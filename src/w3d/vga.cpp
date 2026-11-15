// w3d/vga.cpp — VGAGRAPH decoder (see vga.hpp for the format).
#include "w3d/vga.hpp"

#include <algorithm>
#include <fstream>
#include <string>

namespace w3d {
namespace {

[[nodiscard]] bool read_file(const std::filesystem::path& path,
                             std::vector<std::uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    f.seekg(0, std::ios::end);
    const std::streamsize sz = f.tellg();
    if (sz < 0)
        return false;
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<std::size_t>(sz));
    if (sz > 0 && !f.read(reinterpret_cast<char*>(out.data()), sz))
        return false;
    return true;
}

[[nodiscard]] std::unexpected<Error> fail(Error::Kind k, std::string msg) {
    return std::unexpected(make_error(k, std::move(msg)));
}

[[nodiscard]] std::uint32_t u24(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16);
}

[[nodiscard]] std::uint32_t u32le(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

// Huffman expansion: bits LSB-first, root node 254. Fills `out` with exactly
// `out_len` bytes or returns false when the stream runs short.
bool huff_expand(std::span<const std::uint8_t> data,
                 const std::array<std::array<std::uint16_t, 2>, 255>& nodes,
                 std::size_t out_len, std::vector<std::uint8_t>& out) {
    out.clear();
    out.reserve(out_len);
    std::size_t node = 254;
    std::size_t bit = 0;
    for (std::size_t i = 0; out.size() < out_len; ++i) {
        if (i >= data.size())
            return false;
        const std::uint8_t b = data[i];
        for (; bit < 8 && out.size() < out_len; ++bit) {
            const bool one = (b >> bit) & 1u;
            const std::uint16_t child = nodes[node][one ? 1 : 0];
            if (child < 256) {
                out.push_back(static_cast<std::uint8_t>(child));
                node = 254;
            } else {
                node = child - 256;
            }
        }
        bit = 0;
    }
    return true;
}

// Greedy Huffman expansion to the end of `data` (the TILE8 chunk carries no
// length). Returns the number of decoded bytes.
std::size_t huff_expand_greedy(std::span<const std::uint8_t> data,
                               const std::array<std::array<std::uint16_t, 2>, 255>& nodes,
                               std::vector<std::uint8_t>& out) {
    out.clear();
    std::size_t node = 254;
    for (std::size_t i = 0; i < data.size(); ++i) {
        const std::uint8_t b = data[i];
        for (std::size_t bit = 0; bit < 8; ++bit) {
            const bool one = (b >> bit) & 1u;
            const std::uint16_t child = nodes[node][one ? 1 : 0];
            if (child < 256) {
                out.push_back(static_cast<std::uint8_t>(child));
                node = 254;
            } else {
                node = child - 256;
            }
        }
    }
    return out.size();
}

// Deplane: plane-major 4-bit pixels -> RGBA (A = 255), kVgaGamepal lookup.
void deplane(const std::uint8_t* raw, std::uint16_t w, std::uint16_t h,
             std::vector<std::uint8_t>& rgba) {
    const std::size_t pw = static_cast<std::size_t>(w) / 4;
    rgba.resize(static_cast<std::size_t>(w) * h * 4);
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t cx = 0; cx < pw; ++cx) {
            const std::size_t o = y * pw + cx;
            const std::uint8_t b0 = raw[o];
            const std::uint8_t b1 = raw[h * pw + o];
            const std::uint8_t b2 = raw[2 * h * pw + o];
            const std::uint8_t b3 = raw[3 * h * pw + o];
            for (std::size_t k = 0; k < 4; ++k) {
                const std::uint32_t idx = (((b0 >> k) & 1u)) |
                                          ((((b1 >> k) & 1u)) << 4) |
                                          ((((b2 >> k) & 1u)) << 5) |
                                          ((((b3 >> k) & 1u)) << 6);
                const std::uint32_t c = kVgaGamepal[idx];
                std::uint8_t* p = &rgba[(y * w + cx * 4 + k) * 4];
                p[0] = static_cast<std::uint8_t>(c);
                p[1] = static_cast<std::uint8_t>(c >> 8);
                p[2] = static_cast<std::uint8_t>(c >> 16);
                p[3] = 0xFF;
            }
        }
    }
}

// Decodes a chunk that carries a [u32 expanded] header. `region` starts at
// the chunk's file offset. Returns false if the header is implausible or the
// stream runs short.
bool decode_chunk(std::span<const std::uint8_t> region,
                  const std::array<std::array<std::uint16_t, 2>, 255>& nodes,
                  std::vector<std::uint8_t>& out, std::size_t& expanded) {
    if (region.size() < 4)
        return false;
    const std::uint32_t len = u32le(region.data());
    if (len == 0 || len > 0x40000)  // no VGAGRAPH chunk exceeds 256 KB
        return false;
    if (!huff_expand(region.subspan(4), nodes, len, out))
        return false;
    expanded = len;
    return true;
}

} // namespace

Result<VgaGraph> VgaGraph::load(const std::filesystem::path& vgahead_path,
                                const std::filesystem::path& vgagraph_path,
                                const std::filesystem::path& vgadict_path) {
    VgaGraph g;
    std::vector<std::uint8_t> head, graph, dict;
    if (!read_file(vgahead_path, head) || !read_file(vgagraph_path, graph) ||
        !read_file(vgadict_path, dict)) {
        return fail(Error::Kind::Io, "VGAGRAPH: cannot read VGAHEAD/VGAGRAPH/VGADICT");
    }
    if (dict.size() != 1024)
        return fail(Error::Kind::Corrupt, "VGADICT size != 1024");
    if (head.size() < 6 || head.size() % 3 != 0)
        return fail(Error::Kind::Corrupt, "VGAHEAD size invalid");

    std::array<std::array<std::uint16_t, 2>, 255> nodes;
    for (std::size_t i = 0; i < 255; ++i) {
        const std::uint8_t* p = &dict[4 * i];
        nodes[i][0] = static_cast<std::uint16_t>(p[0]) | (static_cast<std::uint16_t>(p[1]) << 8);
        nodes[i][1] = static_cast<std::uint16_t>(p[2]) | (static_cast<std::uint16_t>(p[3]) << 8);
    }

    // Chunk table: u24 offsets; the last entry must be the EOF sentinel.
    const std::size_t entries = head.size() / 3;
    std::vector<std::uint32_t> offs(entries);
    for (std::size_t i = 0; i < entries; ++i)
        offs[i] = u24(&head[3 * i]);
    if (offs.back() != graph.size())
        return fail(Error::Kind::Corrupt, "VGAHEAD last entry != VGAGRAPH size");
    if (std::any_of(offs.begin(), offs.end(),
                    [&](std::uint32_t v) { return v > graph.size(); }))
        return fail(Error::Kind::Corrupt, "VGAHEAD offset out of range");

    const std::size_t num_chunks = entries - 1;  // exclude the sentinel
    g.numChunks = num_chunks;

    // [start, end) file range of chunk i, skipping sparse successors.
    auto range = [&](std::size_t i) -> std::pair<std::size_t, std::size_t> {
        std::size_t nxt = i + 1;
        while (nxt < num_chunks && offs[nxt] == 0xFFFFFF)
            ++nxt;
        return {offs[i], nxt < num_chunks ? offs[nxt] : offs.back()};
    };

    std::vector<std::uint8_t> data;
    std::size_t expanded = 0;

    // ---- chunk 0: pictable
    {
        auto [a, b] = range(0);
        if (offs[0] == 0xFFFFFF ||
            !decode_chunk({graph.data() + a, b - a}, nodes, data, expanded)) {
            return fail(Error::Kind::Corrupt, "VGAGRAPH chunk 0 (pictable) bad");
        }
        if (data.size() % 4 != 0)
            return fail(Error::Kind::Corrupt, "pictable size not a multiple of 4");
        const std::size_t n_pics = data.size() / 4;
        g.pics.resize(n_pics);
        for (std::size_t i = 0; i < n_pics; ++i) {
            g.pics[i].w = static_cast<std::uint16_t>(data[4 * i]) |
                          (static_cast<std::uint16_t>(data[4 * i + 1]) << 8);
            g.pics[i].h = static_cast<std::uint16_t>(data[4 * i + 2]) |
                          (static_cast<std::uint16_t>(data[4 * i + 3]) << 8);
            if (g.pics[i].w == 0 || g.pics[i].h == 0 || g.pics[i].w > 640 ||
                g.pics[i].h > 400)
                return fail(Error::Kind::Corrupt, "pictable entry implausible");
        }
        if (n_pics + 3 > num_chunks)
            return fail(Error::Kind::Corrupt, "pictable larger than chunk table");
    }

    // ---- chunks 1, 2: fonts
    for (const std::size_t chunk : {std::size_t{1}, std::size_t{2}}) {
        VgaFont* font = (chunk == 1) ? &g.smallFont : &g.bigFont;
        auto [a, b] = range(chunk);
        data.clear();
        if (offs[chunk] == 0xFFFFFF ||
            !decode_chunk({graph.data() + a, b - a}, nodes, data, expanded)) {
            return fail(Error::Kind::Corrupt, "VGAGRAPH font chunk bad");
        }
        if (data.size() < 770 || data[0] == 0 || data[0] > 32)
            return fail(Error::Kind::Corrupt, "font header invalid");
        font->height = data[0];
        for (std::size_t i = 0; i < 256; ++i) {
            const std::uint16_t loc = static_cast<std::uint16_t>(data[2 + 2 * i]) |
                                      (static_cast<std::uint16_t>(data[3 + 2 * i]) << 8);
            const std::uint8_t w = data[514 + i];
            if (w == 0)
                continue;
            const std::size_t need = static_cast<std::size_t>(w) * font->height;
            if (static_cast<std::size_t>(loc) + need > data.size())
                return fail(Error::Kind::Corrupt, "font glyph out of bounds");
            font->glyphs[i].width = w;
            font->glyphs[i].px.assign(data.begin() + loc, data.begin() + loc + need);
        }
    }

    // ---- TILE8 boundary: the single chunk without a u32 header, directly
    // before the trailing run of u32-headered extern chunks. Walk backwards
    // from the last chunk; the first one that rejects the u32 header is the
    // TILE8 chunk (its "u32" is Huffman bits, implausibly large).
    const std::size_t tile8 = [&]() -> std::size_t {
        std::vector<std::uint8_t> tmp;
        std::size_t tmp_exp = 0;
        for (std::size_t j = num_chunks; j > 3;) {
            --j;
            if (offs[j] == 0xFFFFFF)
                continue;
            auto [a, b] = range(j);
            if (!decode_chunk({graph.data() + a, b - a}, nodes, tmp, tmp_exp))
                return j;
        }
        return 0;  // no implicit-size chunk present
    }();

    // ---- pic chunks 3 .. last pic (all before the TILE8 chunk)
    for (std::size_t i = 0; i < g.pics.size(); ++i) {
        const std::size_t chunk = i + 3;
        if (tile8 != 0 && chunk >= tile8)
            break;
        VgaPic& pic = g.pics[i];
        auto [a, b] = range(chunk);
        if (offs[chunk] == 0xFFFFFF)
            continue;  // sparse pic: dims known, no pixel data
        data.clear();
        if (!decode_chunk({graph.data() + a, b - a}, nodes, data, expanded))
            return fail(Error::Kind::Corrupt, "VGAGRAPH pic chunk bad");
        if (expanded != static_cast<std::size_t>(pic.w) * pic.h)
            return fail(Error::Kind::Corrupt, "pic expanded size != w*h");
        deplane(data.data(), pic.w, pic.h, pic.rgba);
    }

    // ---- TILE8 chunk: implicit size, greedy decode
    if (tile8 != 0 && offs[tile8] != 0xFFFFFF) {
        auto [a, b] = range(tile8);
        huff_expand_greedy({graph.data() + a, b - a}, nodes, g.tiles);
    }

    return g;
}

} // namespace w3d
