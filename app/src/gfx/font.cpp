// ps5-homebrew-ui - Baked SDF font: loading, measuring and glyph layout.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gfx/font.hpp"

#include "gfx/script.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

// stb_image's zlib decoder (src/third_party/stb/image.c).
extern "C" int stbi_zlib_decode_buffer(char *output, int output_size, const char *input, int input_size);

namespace hui::gfx
{

namespace ff = font_format;

FontBytes inflate_font(std::string_view packed, std::size_t *size)
{
    *size = 0;
    FontBytes none(nullptr, std::free);
    if (packed.size() < 8 || packed.substr(0, 4) != "HUFZ" || packed.size() > 0x7fffffffu)
        return none;
    std::uint32_t expected = 0;
    std::memcpy(&expected, packed.data() + 4, sizeof(expected));
    if (expected == 0 || expected > 128u * 1024u * 1024u)
        return none;
    // Bundled language fonts have a fixed decoded size. Bound allocation and
    // decoding so a damaged asset cannot grow an unbounded decompression buffer.
    FontBytes bytes(static_cast<char *>(std::malloc(expected)), std::free);
    if (bytes == nullptr)
        return none;
    const int inflated = stbi_zlib_decode_buffer(bytes.get(), static_cast<int>(expected), packed.data() + 8,
                                                static_cast<int>(packed.size() - 8));
    if (inflated < 0 || static_cast<std::uint32_t>(inflated) != expected)
        return none;
    *size = expected;
    return bytes;
}

std::uint32_t next_codepoint(std::string_view text, std::size_t *index)
{
    const auto byte = [&](std::size_t at) { return static_cast<unsigned char>(text[at]); };
    const std::size_t i = *index;
    const unsigned char lead = byte(i);
    int length = 1;
    std::uint32_t value = lead;
    if (lead >= 0xf0 && lead < 0xf8)
    {
        length = 4;
        value = lead & 0x07u;
    }
    else if (lead >= 0xe0)
    {
        length = 3;
        value = lead & 0x0fu;
    }
    else if (lead >= 0xc0)
    {
        length = 2;
        value = lead & 0x1fu;
    }
    else if (lead >= 0x80)
    {
        *index = i + 1;
        return 0xfffd;
    }
    if (i + static_cast<std::size_t>(length) > text.size())
    {
        *index = text.size();
        return 0xfffd;
    }
    for (int k = 1; k < length; ++k)
    {
        const unsigned char continuation = byte(i + static_cast<std::size_t>(k));
        if ((continuation & 0xc0u) != 0x80u)
        {
            *index = i + 1;
            return 0xfffd;
        }
        value = (value << 6) | (continuation & 0x3fu);
    }
    *index = i + static_cast<std::size_t>(length);
    return value;
}

bool Font::load(std::string_view data, bool copy_atlas)
{
    error_.clear();
    if (data.size() < sizeof(ff::Header))
    {
        error_ = "font too small";
        return false;
    }
    std::memcpy(&header_, data.data(), sizeof(header_));
    if (header_.magic != ff::kMagic || header_.version != ff::kVersion ||
        header_.pixel_size <= 0.0f)
    {
        error_ = "not a huifont v1";
        return false;
    }
    const std::size_t glyph_bytes =
        static_cast<std::size_t>(header_.glyph_count) * sizeof(ff::Glyph);
    const std::size_t kern_bytes = static_cast<std::size_t>(header_.kern_count) * sizeof(ff::Kern);
    const std::size_t atlas_bytes = static_cast<std::size_t>(header_.atlas_width) *
                                    static_cast<std::size_t>(header_.atlas_height);
    if (data.size() != sizeof(ff::Header) + glyph_bytes + kern_bytes + atlas_bytes)
    {
        error_ = "font size mismatch";
        return false;
    }
    const char *cursor = data.data() + sizeof(ff::Header);
    // A font may have no glyphs or (monospaced faces) no kerning pairs.
    glyphs_.resize(header_.glyph_count);
    if (glyph_bytes != 0)
        std::memcpy(glyphs_.data(), cursor, glyph_bytes);
    cursor += glyph_bytes;
    kerns_.resize(header_.kern_count);
    if (kern_bytes != 0)
        std::memcpy(kerns_.data(), cursor, kern_bytes);
    cursor += kern_bytes;
    borrowed_atlas_ = nullptr;
    if (copy_atlas)
        atlas_.assign(reinterpret_cast<const std::uint8_t *>(cursor),
                      reinterpret_cast<const std::uint8_t *>(cursor) + atlas_bytes);
    else
    {
        std::vector<std::uint8_t>().swap(atlas_);
        borrowed_atlas_ = reinterpret_cast<const std::uint8_t *>(cursor);
    }
    for (const ff::Glyph &glyph : glyphs_)
    {
        if (glyph.x + glyph.w > header_.atlas_width || glyph.y + glyph.h > header_.atlas_height)
        {
            error_ = "glyph outside atlas";
            return false;
        }
    }
    return true;
}

const ff::Glyph *Font::find(std::uint32_t codepoint) const
{
    const auto it =
        std::lower_bound(glyphs_.begin(), glyphs_.end(), codepoint,
                         [](const ff::Glyph &g, std::uint32_t c) { return g.codepoint < c; });
    return it != glyphs_.end() && it->codepoint == codepoint ? &*it : nullptr;
}

Font::Resolved Font::resolve(std::uint32_t codepoint) const
{
    if (const ff::Glyph *glyph = find(codepoint))
        return {glyph, this};
    if (fallback_ != nullptr)
    {
        if (const ff::Glyph *glyph = fallback_->find(codepoint))
            return {glyph, fallback_};
    }
    if (const ff::Glyph *glyph = find('?'))
        return {glyph, this};
    return {};
}

float Font::kern(std::uint32_t first, std::uint32_t second) const
{
    const auto it = std::lower_bound(
        kerns_.begin(), kerns_.end(), std::make_pair(first, second),
        [](const ff::Kern &k, const std::pair<std::uint32_t, std::uint32_t> &key)
        { return k.first != key.first ? k.first < key.first : k.second < key.second; });
    return it != kerns_.end() && it->first == first && it->second == second ? it->amount : 0.0f;
}

std::string Font::fit(std::string_view text, float size, float max_width, float tracking) const
{
    if (measure(text, size, tracking) <= max_width)
        return std::string(text);
    constexpr std::string_view kEllipsis = "\xE2\x80\xA6";
    const std::string_view mark = has_glyph(0x2026) ? kEllipsis : std::string_view("...");
    std::string best;
    for (std::size_t index = 0; index < text.size();)
    {
        const std::size_t start = index;
        next_codepoint(text, &index);
        std::string candidate(text.substr(0, start));
        while (!candidate.empty() && candidate.back() == ' ')
            candidate.pop_back();
        candidate.append(mark);
        if (measure(candidate, size, tracking) > max_width)
            break;
        best = std::move(candidate);
    }
    return best;
}

float Font::measure(std::string_view text, float size, float tracking) const
{
    // Joined Arabic letters have their own widths; order doesn't matter here.
    return has_right_to_left(text) ? measure_shaped(shape(text), size, tracking)
                                   : measure_shaped(text, size, tracking);
}

float Font::measure_shaped(std::string_view text, float size, float tracking) const
{
    // Letter spacing separates joined Arabic forms. Keep script joins intact.
    if (has_right_to_left(text)) tracking = 0.0f;
    float width = 0.0f;
    std::uint32_t previous = 0;
    const Font *previous_font = nullptr;
    int glyphs = 0;
    for (std::size_t index = 0; index < text.size();)
    {
        const std::uint32_t codepoint = next_codepoint(text, &index);
        const Resolved found = resolve(codepoint);
        if (found.glyph == nullptr)
            continue;
        const float scale = size / found.font->header_.pixel_size;
        if (previous != 0 && previous_font == found.font)
            width += found.font->kern(previous, found.glyph->codepoint) * scale;
        width += found.glyph->advance * scale;
        previous = found.glyph->codepoint;
        previous_font = found.font;
        ++glyphs;
    }
    return glyphs > 1 ? width + tracking * static_cast<float>(glyphs - 1) : width;
}

float Font::layout(std::string_view logical, float x, float y, float size, Align align,
                   std::vector<GlyphQuad> &quads, float tracking) const
{
    if (has_right_to_left(logical)) tracking = 0.0f;
    // Right-to-left text is laid out in the order it is displayed.
    const std::string reordered = has_right_to_left(logical) ? visual(logical) : std::string();
    const std::string_view text = reordered.empty() ? logical : std::string_view(reordered);
    const float width = measure_shaped(text, size, tracking);
    float pen = x;
    if (align == Align::center)
        pen -= width * 0.5f;
    else if (align == Align::right)
        pen -= width;
    std::uint32_t previous = 0;
    const Font *previous_font = nullptr;
    for (std::size_t index = 0; index < text.size();)
    {
        const std::uint32_t codepoint = next_codepoint(text, &index);
        const Resolved found = resolve(codepoint);
        if (found.glyph == nullptr)
            continue;
        const ff::Glyph *glyph = found.glyph;
        const ff::Header &header = found.font->header_;
        const float scale = size / header.pixel_size;
        if (previous != 0 && previous_font == found.font)
            pen += found.font->kern(previous, glyph->codepoint) * scale;
        if (glyph->w > 0 && glyph->h > 0)
        {
            const float inverse_w = 1.0f / static_cast<float>(header.atlas_width);
            const float inverse_h = 1.0f / static_cast<float>(header.atlas_height);
            GlyphQuad quad;
            quad.x0 = pen + glyph->offset_x * scale;
            quad.y0 = y + glyph->offset_y * scale;
            quad.x1 = quad.x0 + static_cast<float>(glyph->w) * scale;
            quad.y1 = quad.y0 + static_cast<float>(glyph->h) * scale;
            quad.u0 = static_cast<float>(glyph->x) * inverse_w;
            quad.v0 = static_cast<float>(glyph->y) * inverse_h;
            quad.u1 = static_cast<float>(glyph->x + glyph->w) * inverse_w;
            quad.v1 = static_cast<float>(glyph->y + glyph->h) * inverse_h;
            quad.fallback = found.font != this;
            quads.push_back(quad);
        }
        pen += glyph->advance * scale + tracking;
        previous = glyph->codepoint;
        previous_font = found.font;
    }
    return width;
}

std::vector<std::string> Font::wrap(std::string_view text, float size, float max_width) const
{
    const auto trimmed = [](std::string s) {
        while (!s.empty() && s.back() == ' ')
            s.pop_back();
        return s;
    };
    std::vector<std::string> lines;
    std::size_t index = 0;
    while (index <= text.size())
    {
        const std::size_t newline = text.find('\n', index);
        const std::string_view paragraph = text.substr(
            index, newline == std::string_view::npos ? std::string_view::npos : newline - index);
        // Pieces end where a line may break: after spaces, and between
        // Chinese and Japanese characters, which have no spaces.
        std::string line, piece;
        std::uint32_t previous = 0;
        const auto place = [&] {
            if (piece.empty())
                return;
            if (!line.empty() && measure(trimmed(line + piece), size) > max_width)
            {
                lines.push_back(trimmed(line));
                std::size_t start = piece.find_first_not_of(' ');
                line = start == std::string::npos ? std::string() : piece.substr(start);
            }
            else
                line += piece;
            piece.clear();
        };
        for (std::size_t at = 0; at < paragraph.size();)
        {
            const std::size_t start = at;
            const std::uint32_t codepoint = next_codepoint(paragraph, &at);
            if (previous != 0 && codepoint != ' ' && breaks_between(previous, codepoint))
                place();
            piece.append(paragraph.substr(start, at - start));
            previous = codepoint;
        }
        place();
        lines.push_back(trimmed(line));
        if (newline == std::string_view::npos)
            break;
        index = newline + 1;
    }
    return lines;
}

} // namespace hui::gfx
