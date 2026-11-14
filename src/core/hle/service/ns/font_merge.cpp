// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstring>
#include <map>
#include <optional>
#include <string>

#include "core/hle/service/ns/font_merge.h"

namespace Service::NS {

namespace {

using Bytes = std::vector<std::uint8_t>;
using View = std::span<const std::uint8_t>;

std::uint16_t Be16(View data, std::size_t offset) {
    return static_cast<std::uint16_t>((data[offset] << 8) | data[offset + 1]);
}

std::uint32_t Be32(View data, std::size_t offset) {
    return (std::uint32_t{data[offset]} << 24) | (std::uint32_t{data[offset + 1]} << 16) |
           (std::uint32_t{data[offset + 2]} << 8) | data[offset + 3];
}

void Put16(Bytes& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

void Put32(Bytes& out, std::uint32_t value) {
    Put16(out, value >> 16);
    Put16(out, value & 0xffff);
}

void Set16(Bytes& data, std::size_t offset, std::uint32_t value) {
    data[offset] = static_cast<std::uint8_t>(value >> 8);
    data[offset + 1] = static_cast<std::uint8_t>(value);
}

void Set32(Bytes& data, std::size_t offset, std::uint32_t value) {
    Set16(data, offset, value >> 16);
    Set16(data, offset + 2, value & 0xffff);
}

struct Font {
    std::map<std::string, View> tables;

    View Table(const char* tag) const {
        const auto it = tables.find(tag);
        return it == tables.end() ? View{} : it->second;
    }
};

std::optional<Font> ParseFont(View data) {
    if (data.size() < 12) {
        return std::nullopt;
    }
    const std::uint32_t version = Be32(data, 0);
    if (version != 0x00010000 && version != 0x74727565) { // 1.0 or 'true'
        return std::nullopt;
    }
    const std::size_t count = Be16(data, 4);
    if (data.size() < 12 + count * 16) {
        return std::nullopt;
    }
    Font font;
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t entry = 12 + i * 16;
        const std::string tag(reinterpret_cast<const char*>(data.data() + entry), 4);
        const std::size_t offset = Be32(data, entry + 8);
        const std::size_t length = Be32(data, entry + 12);
        if (offset > data.size() || length > data.size() - offset) {
            return std::nullopt;
        }
        font.tables[tag] = data.subspan(offset, length);
    }
    return font;
}

/// The Unicode BMP character map of a font (format 4 subtable, platform 3 or 0).
std::optional<std::map<std::uint32_t, std::uint16_t>> ReadCmap(View cmap) {
    if (cmap.size() < 4) {
        return std::nullopt;
    }
    const std::size_t subtables = Be16(cmap, 2);
    for (std::size_t i = 0; i < subtables && 4 + (i + 1) * 8 <= cmap.size(); ++i) {
        const std::size_t record = 4 + i * 8;
        const std::uint16_t platform = Be16(cmap, record);
        const std::uint16_t encoding = Be16(cmap, record + 2);
        const std::size_t offset = Be32(cmap, record + 4);
        if (!((platform == 3 && encoding == 1) || platform == 0) || offset + 14 > cmap.size() ||
            Be16(cmap, offset) != 4) {
            continue;
        }
        const std::size_t length = Be16(cmap, offset + 2);
        if (offset + length > cmap.size()) {
            return std::nullopt;
        }
        const View sub = cmap.subspan(offset, length);
        const std::size_t segments = Be16(sub, 6) / 2;
        const std::size_t ends = 14;
        const std::size_t starts = ends + segments * 2 + 2;
        const std::size_t deltas = starts + segments * 2;
        const std::size_t ranges = deltas + segments * 2;
        if (ranges + segments * 2 > sub.size()) {
            return std::nullopt;
        }
        std::map<std::uint32_t, std::uint16_t> map;
        for (std::size_t s = 0; s < segments; ++s) {
            const std::uint32_t end = Be16(sub, ends + s * 2);
            const std::uint32_t start = Be16(sub, starts + s * 2);
            const std::uint16_t delta = Be16(sub, deltas + s * 2);
            const std::uint16_t range = Be16(sub, ranges + s * 2);
            for (std::uint32_t c = start; c <= end && c != 0xffff; ++c) {
                std::uint32_t glyph;
                if (range == 0) {
                    glyph = (c + delta) & 0xffff;
                } else {
                    const std::size_t at = ranges + s * 2 + range + (c - start) * 2;
                    if (at + 2 > sub.size()) {
                        return std::nullopt;
                    }
                    glyph = Be16(sub, at);
                    if (glyph != 0) {
                        glyph = (glyph + delta) & 0xffff;
                    }
                }
                if (glyph != 0) {
                    map[c] = static_cast<std::uint16_t>(glyph);
                }
            }
        }
        return map;
    }
    return std::nullopt;
}

/// A format 4 character map with one entry per character, written as a Unicode (0, 3) and a
/// Windows (3, 1) subtable that share the same data.
std::optional<Bytes> WriteCmap(const std::map<std::uint32_t, std::uint16_t>& map) {
    struct Segment {
        std::uint32_t start;
        std::uint32_t end;
        std::size_t first_index;
    };
    std::vector<Segment> segments;
    std::vector<std::uint16_t> glyphs;
    for (const auto& [codepoint, glyph] : map) {
        if (codepoint >= 0xffff) {
            continue;
        }
        if (segments.empty() || segments.back().end + 1 != codepoint) {
            segments.push_back({codepoint, codepoint, glyphs.size()});
        } else {
            segments.back().end = codepoint;
        }
        glyphs.push_back(glyph);
    }
    segments.push_back({0xffff, 0xffff, glyphs.size()});
    const std::size_t count = segments.size();
    const std::size_t length = 16 + count * 8 + glyphs.size() * 2;
    if (length > 0xffff || count > 0x7fff) {
        return std::nullopt;
    }
    std::uint32_t search_range = 2;
    std::uint32_t entry_selector = 1;
    while (search_range * 2 <= count * 2) {
        search_range *= 2;
        ++entry_selector;
    }
    --entry_selector;
    Bytes sub;
    Put16(sub, 4);
    Put16(sub, static_cast<std::uint32_t>(length));
    Put16(sub, 0);
    Put16(sub, static_cast<std::uint32_t>(count * 2));
    Put16(sub, search_range);
    Put16(sub, entry_selector);
    Put16(sub, static_cast<std::uint32_t>(count * 2 - search_range));
    for (const auto& segment : segments) {
        Put16(sub, segment.end);
    }
    Put16(sub, 0);
    for (const auto& segment : segments) {
        Put16(sub, segment.start);
    }
    for (const auto& segment : segments) {
        Put16(sub, segment.start == 0xffff ? 1 : 0); // idDelta
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (segments[i].start == 0xffff) {
            Put16(sub, 0);
        } else {
            // Offset from this idRangeOffset entry to the glyph array entry of the first character.
            Put16(sub, static_cast<std::uint32_t>((count - i) * 2 + segments[i].first_index * 2));
        }
    }
    for (const auto glyph : glyphs) {
        Put16(sub, glyph);
    }
    Bytes out;
    Put16(out, 0);
    Put16(out, 2);
    Put16(out, 0);
    Put16(out, 3);
    Put32(out, 20);
    Put16(out, 3);
    Put16(out, 1);
    Put32(out, 20);
    out.insert(out.end(), sub.begin(), sub.end());
    return out;
}

std::uint32_t Checksum(View data) {
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < data.size(); i += 4) {
        std::uint32_t word = 0;
        for (std::size_t j = 0; j < 4; ++j) {
            word = (word << 8) | (i + j < data.size() ? data[i + j] : 0);
        }
        sum += word;
    }
    return sum;
}

struct GlyphData {
    View outline;
    std::uint16_t advance;
    std::int16_t lsb;
};

/// The outline of glyph `id` and its horizontal metrics, or nullopt if missing or unreadable.
std::optional<GlyphData> ReadGlyph(const Font& font, std::uint16_t id) {
    const View head = font.Table("head");
    const View loca = font.Table("loca");
    const View glyf = font.Table("glyf");
    const View hhea = font.Table("hhea");
    const View hmtx = font.Table("hmtx");
    const View maxp = font.Table("maxp");
    if (head.size() < 54 || hhea.size() < 36 || maxp.size() < 6) {
        return std::nullopt;
    }
    const std::size_t glyphs = Be16(maxp, 4);
    const bool long_loca = Be16(head, 50) != 0;
    if (id >= glyphs || loca.size() < (glyphs + 1) * (long_loca ? 4 : 2)) {
        return std::nullopt;
    }
    const auto location = [&](std::size_t index) -> std::size_t {
        return long_loca ? Be32(loca, index * 4) : std::size_t{Be16(loca, index * 2)} * 2;
    };
    const std::size_t begin = location(id);
    const std::size_t end = location(id + 1);
    if (end < begin || end > glyf.size() || end - begin < 10) {
        return std::nullopt;
    }
    const std::size_t metrics = Be16(hhea, 34);
    if (metrics == 0 || hmtx.size() < metrics * 4) {
        return std::nullopt;
    }
    GlyphData glyph{glyf.subspan(begin, end - begin), 0, 0};
    if (id < metrics) {
        glyph.advance = Be16(hmtx, id * 4);
        glyph.lsb = static_cast<std::int16_t>(Be16(hmtx, id * 4 + 2));
    } else {
        const std::size_t at = metrics * 4 + (id - metrics) * 2;
        if (at + 2 > hmtx.size()) {
            return std::nullopt;
        }
        glyph.advance = Be16(hmtx, (metrics - 1) * 4);
        glyph.lsb = static_cast<std::int16_t>(Be16(hmtx, at));
    }
    return glyph;
}

/// Points, contours and instruction size of a simple glyph, or nullopt for a composite one.
struct GlyphLimits {
    std::uint16_t points;
    std::uint16_t contours;
    std::uint16_t instructions;
};

std::optional<GlyphLimits> SimpleGlyphLimits(View outline) {
    const std::int16_t contours = static_cast<std::int16_t>(Be16(outline, 0));
    if (contours <= 0 || outline.size() < 12 + static_cast<std::size_t>(contours) * 2) {
        return std::nullopt;
    }
    const std::size_t last_end = 10 + (static_cast<std::size_t>(contours) - 1) * 2;
    const std::uint16_t points = static_cast<std::uint16_t>(Be16(outline, last_end) + 1);
    const std::uint16_t instructions = Be16(outline, 10 + static_cast<std::size_t>(contours) * 2);
    return GlyphLimits{points, static_cast<std::uint16_t>(contours), instructions};
}

} // Anonymous namespace

Bytes MergeGlyphs(View base_data, View donor_data, std::span<const std::uint32_t> codepoints) {
    const auto base = ParseFont(base_data);
    const auto donor = ParseFont(donor_data);
    if (!base || !donor) {
        return {};
    }
    for (const char* tag : {"head", "maxp", "loca", "glyf", "hhea", "hmtx", "cmap"}) {
        if (base->Table(tag).empty() || donor->Table(tag).empty()) {
            return {};
        }
    }
    const View head = base->Table("head");
    const View maxp = base->Table("maxp");
    const View hhea = base->Table("hhea");
    if (head.size() < 54 || maxp.size() < 32 || hhea.size() < 36 ||
        Be16(head, 18) != Be16(donor->Table("head"), 18)) {
        return {};
    }

    auto base_map = ReadCmap(base->Table("cmap"));
    const auto donor_map = ReadCmap(donor->Table("cmap"));
    if (!base_map || !donor_map) {
        return {};
    }

    struct Added {
        std::uint32_t codepoint;
        GlyphData glyph;
        GlyphLimits limits;
    };
    std::vector<Added> added;
    for (const std::uint32_t codepoint : codepoints) {
        if (codepoint >= 0xffff || base_map->contains(codepoint)) {
            continue;
        }
        const auto found = donor_map->find(codepoint);
        if (found == donor_map->end()) {
            continue;
        }
        const auto glyph = ReadGlyph(*donor, found->second);
        if (!glyph) {
            continue;
        }
        const auto limits = SimpleGlyphLimits(glyph->outline);
        if (!limits) {
            continue;
        }
        added.push_back({codepoint, *glyph, *limits});
    }
    if (added.empty()) {
        return {};
    }

    const std::size_t old_glyphs = Be16(maxp, 4);
    const std::size_t new_glyphs = old_glyphs + added.size();
    if (new_glyphs > 0xffff) {
        return {};
    }
    const bool long_loca = Be16(head, 50) != 0;
    const View old_loca = base->Table("loca");
    if (old_loca.size() < (old_glyphs + 1) * (long_loca ? 4 : 2)) {
        return {};
    }
    const View old_glyf = base->Table("glyf");

    // glyf and loca: the old outlines untouched, the new ones appended on 4-byte boundaries.
    Bytes glyf(old_glyf.begin(), old_glyf.end());
    std::vector<std::uint32_t> offsets(old_glyphs + 1);
    for (std::size_t i = 0; i <= old_glyphs; ++i) {
        offsets[i] = long_loca ? Be32(old_loca, i * 4) : Be16(old_loca, i * 2) * 2u;
    }
    // Glyphs start on even offsets. The table is not padded further, so that the last original glyph
    // (an empty one in some fonts) keeps its length.
    glyf.resize((glyf.size() + 1) & ~std::size_t{1});
    offsets[old_glyphs] = static_cast<std::uint32_t>(glyf.size());
    for (const auto& item : added) {
        glyf.insert(glyf.end(), item.glyph.outline.begin(), item.glyph.outline.end());
        glyf.resize((glyf.size() + 1) & ~std::size_t{1});
        offsets.push_back(static_cast<std::uint32_t>(glyf.size()));
    }
    Bytes loca;
    for (const auto offset : offsets) {
        Put32(loca, offset);
    }

    // Metrics: every glyph gets a full (advance, side bearing) pair so the new advances fit.
    const auto widen = [&](View metrics, View header, std::uint16_t fallback_advance,
                           const auto& new_values) -> std::optional<Bytes> {
        const std::size_t long_count = Be16(header, 34);
        if (long_count == 0 || metrics.size() < long_count * 4) {
            return std::nullopt;
        }
        Bytes out;
        for (std::size_t i = 0; i < old_glyphs; ++i) {
            if (i < long_count) {
                if ((i + 1) * 4 > metrics.size()) {
                    return std::nullopt;
                }
                Put16(out, Be16(metrics, i * 4));
                Put16(out, Be16(metrics, i * 4 + 2));
            } else {
                const std::size_t at = long_count * 4 + (i - long_count) * 2;
                if (at + 2 > metrics.size()) {
                    return std::nullopt;
                }
                Put16(out, Be16(metrics, (long_count - 1) * 4));
                Put16(out, Be16(metrics, at));
            }
        }
        for (const auto& value : new_values) {
            Put16(out, value.first ? value.first : fallback_advance);
            Put16(out, static_cast<std::uint16_t>(value.second));
        }
        return out;
    };
    std::vector<std::pair<std::uint16_t, std::int16_t>> horizontal;
    for (const auto& item : added) {
        horizontal.emplace_back(item.glyph.advance, item.glyph.lsb);
    }
    const auto hmtx = widen(base->Table("hmtx"), hhea, 1024, horizontal);
    if (!hmtx) {
        return {};
    }
    std::optional<Bytes> vmtx;
    const View vhea = base->Table("vhea");
    if (!vhea.empty()) {
        if (vhea.size() < 36 || base->Table("vmtx").empty()) {
            return {};
        }
        // The donor has no vertical metrics: a full em advance, top side bearing 0.
        std::vector<std::pair<std::uint16_t, std::int16_t>> vertical(
            added.size(), {Be16(head, 18), std::int16_t{0}});
        vmtx = widen(base->Table("vmtx"), vhea, Be16(head, 18), vertical);
        if (!vmtx) {
            return {};
        }
    }

    // The new glyphs follow the old ones in the order added.
    std::size_t next_glyph = old_glyphs;
    for (const auto& item : added) {
        (*base_map)[item.codepoint] = static_cast<std::uint16_t>(next_glyph++);
    }
    const auto cmap = WriteCmap(*base_map);
    if (!cmap) {
        return {};
    }

    // Tables that change; every other table is copied as it is.
    std::map<std::string, Bytes> changed;
    changed["glyf"] = std::move(glyf);
    changed["loca"] = std::move(loca);
    changed["hmtx"] = *hmtx;
    changed["cmap"] = *cmap;

    Bytes new_head(head.begin(), head.end());
    Set32(new_head, 8, 0); // checkSumAdjustment, set below
    Set16(new_head, 50, 1);
    changed["head"] = std::move(new_head);

    Bytes new_maxp(maxp.begin(), maxp.end());
    Set16(new_maxp, 4, static_cast<std::uint32_t>(new_glyphs));
    std::uint16_t max_points = Be16(maxp, 6);
    std::uint16_t max_contours = Be16(maxp, 8);
    std::uint16_t max_instructions = Be16(maxp, 26);
    for (const auto& item : added) {
        max_points = std::max(max_points, item.limits.points);
        max_contours = std::max(max_contours, item.limits.contours);
        max_instructions = std::max(max_instructions, item.limits.instructions);
    }
    Set16(new_maxp, 6, max_points);
    Set16(new_maxp, 8, max_contours);
    Set16(new_maxp, 26, max_instructions);
    changed["maxp"] = std::move(new_maxp);

    Bytes new_hhea(hhea.begin(), hhea.end());
    Set16(new_hhea, 34, static_cast<std::uint32_t>(new_glyphs));
    std::uint16_t advance_max = Be16(hhea, 10);
    for (const auto& item : added) {
        advance_max = std::max(advance_max, item.glyph.advance);
    }
    Set16(new_hhea, 10, advance_max);
    changed["hhea"] = std::move(new_hhea);

    if (vmtx) {
        Bytes new_vhea(vhea.begin(), vhea.end());
        Set16(new_vhea, 34, static_cast<std::uint32_t>(new_glyphs));
        changed["vhea"] = std::move(new_vhea);
        changed["vmtx"] = std::move(*vmtx);
    }

    const View post = base->Table("post");
    if (post.size() >= 32) {
        // Glyph names are not needed to draw text: use version 3.0, which has none.
        Bytes new_post(post.begin(), post.begin() + 32);
        Set32(new_post, 0, 0x00030000);
        changed["post"] = std::move(new_post);
    }

    // Assemble the font with the tables in tag order.
    std::map<std::string, View> all = base->tables;
    for (const auto& [tag, data] : changed) {
        all[tag] = data;
    }
    const std::size_t count = all.size();
    std::uint32_t search_range = 16;
    std::uint32_t entry_selector = 0;
    while (search_range * 2 <= count * 16) {
        search_range *= 2;
        ++entry_selector;
    }
    Bytes out;
    Put32(out, 0x00010000);
    Put16(out, static_cast<std::uint32_t>(count));
    Put16(out, search_range);
    Put16(out, entry_selector);
    Put16(out, static_cast<std::uint32_t>(count * 16 - search_range));
    std::size_t data_offset = 12 + count * 16;
    for (const auto& [tag, data] : all) {
        for (const char c : tag) {
            out.push_back(static_cast<std::uint8_t>(c));
        }
        Put32(out, Checksum(data));
        Put32(out, static_cast<std::uint32_t>(data_offset));
        Put32(out, static_cast<std::uint32_t>(data.size()));
        data_offset += (data.size() + 3) & ~std::size_t{3};
    }
    std::size_t head_offset = 0;
    for (const auto& [tag, data] : all) {
        if (tag == "head") {
            head_offset = out.size();
        }
        out.insert(out.end(), data.begin(), data.end());
        out.resize((out.size() + 3) & ~std::size_t{3});
    }
    // head.checkSumAdjustment makes the whole file sum to 0xb1b0afba.
    Set32(out, head_offset + 8, 0);
    Set32(out, head_offset + 8, 0xb1b0afba - Checksum(out));
    return out;
}

} // namespace Service::NS
