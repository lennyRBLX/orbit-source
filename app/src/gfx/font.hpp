// ps5-homebrew-ui - Baked SDF font: loading, measuring and glyph layout.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "gfx/font_format.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace hui::gfx
{

// One positioned glyph quad in output pixels, with its atlas UV rectangle.
struct GlyphQuad
{
    float x0, y0, x1, y1;
    float u0, v0, u1, v1;
    bool fallback = false; // from the fallback font's atlas
};

enum class Align : std::uint8_t
{
    left,
    center,
    right,
};

class Font
{
  public:
    // Parses a .huifont blob; returns false (and keeps an error) if invalid.
    // Without copy_atlas the atlas pixels stay in data, for an upload while
    // data is alive (large CJK fonts); call forget_atlas() after it.
    bool load(std::string_view data, bool copy_atlas = true);
    const std::string &error() const
    {
        return error_;
    }

    // Width in pixels of one line of UTF-8 text at the given pixel size.
    // tracking is extra space between glyphs, in pixels.
    float measure(std::string_view text, float size, float tracking = 0.0f) const;
    // text, or its longest prefix plus an ellipsis that fits max_width.
    std::string fit(std::string_view text, float size, float max_width,
                    float tracking = 0.0f) const;
    float ascent(float size) const
    {
        return header_.ascent * size / header_.pixel_size;
    }
    float descent(float size) const
    {
        return -header_.descent * size / header_.pixel_size;
    }
    float line_height(float size) const
    {
        return (header_.ascent - header_.descent + header_.line_gap) * size / header_.pixel_size;
    }
    // Distance-field spread in output pixels at a size (for shader anti-aliasing).
    float sdf_range(float size) const
    {
        return header_.sdf_range * size / header_.pixel_size;
    }

    // Lays out one line with its baseline at y. x is the left edge, centre or
    // right edge depending on align. Appends to quads; returns the advance.
    float layout(std::string_view text, float x, float y, float size, Align align,
                 std::vector<GlyphQuad> &quads, float tracking = 0.0f) const;

    // Breaks text into lines no wider than max_width: at spaces, and between
    // Chinese and Japanese characters (script.hpp).
    std::vector<std::string> wrap(std::string_view text, float size, float max_width) const;

    // Characters this font lacks come from another baked font (CJK, Arabic),
    // drawn from its own atlas texture. nullptr removes it.
    void set_fallback(const Font *font, std::uint32_t texture)
    {
        fallback_ = font;
        fallback_texture_ = texture;
    }
    const Font *fallback() const
    {
        return fallback_;
    }
    std::uint32_t fallback_texture() const
    {
        return fallback_texture_;
    }


    std::uint16_t atlas_width() const
    {
        return header_.atlas_width;
    }
    std::uint16_t atlas_height() const
    {
        return header_.atlas_height;
    }
    const std::vector<std::uint8_t> &atlas() const
    {
        return atlas_;
    }
    // The atlas pixels to upload, copied or still in the loaded data.
    const std::uint8_t *atlas_pixels() const
    {
        return atlas_.empty() ? borrowed_atlas_ : atlas_.data();
    }
    // After the upload: no pixels kept in memory, only the metrics.
    void forget_atlas()
    {
        std::vector<std::uint8_t>().swap(atlas_);
        borrowed_atlas_ = nullptr;
    }
    bool has_glyph(std::uint32_t codepoint) const
    {
        return find(codepoint) != nullptr;
    }

  private:
    struct Resolved
    {
        const font_format::Glyph *glyph = nullptr;
        const Font *font = nullptr;
    };
    const font_format::Glyph *find(std::uint32_t codepoint) const;
    // This font's glyph, the fallback's, or a question mark.
    Resolved resolve(std::uint32_t codepoint) const;
    float kern(std::uint32_t first, std::uint32_t second) const;
    float measure_shaped(std::string_view text, float size, float tracking) const;

    const Font *fallback_ = nullptr;
    std::uint32_t fallback_texture_ = 0;
    const std::uint8_t *borrowed_atlas_ = nullptr;

    font_format::Header header_{};
    std::vector<font_format::Glyph> glyphs_;
    std::vector<font_format::Kern> kerns_;
    std::vector<std::uint8_t> atlas_;
    std::string error_;
};

// Decodes the next UTF-8 codepoint from text at *index (advancing it);
// invalid bytes decode as U+FFFD.
std::uint32_t next_codepoint(std::string_view text, std::size_t *index);

// A .huifontz file: "HUFZ", the .huifont's size (uint32, little-endian), then
// the .huifont as one zlib stream (tools/bake-fonts.sh). Inflates it into a
// buffer of exactly that size, so no larger copy is ever made; nullptr when
// the file is not one.
using FontBytes = std::unique_ptr<char, void (*)(void *)>;
FontBytes inflate_font(std::string_view packed, std::size_t *size);

// The largest font file (.huifont or .huifontz) read whole, beyond a plain
// file read's limit: the bundled ones go up to 15.8 MB (Traditional Chinese).
inline constexpr std::size_t kFontFileLimit = 32u << 20;

} // namespace hui::gfx
