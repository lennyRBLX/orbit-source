// Orbit Store TV app - Scripts beyond Latin: Arabic joining and right-to-left
// order, Chinese and Japanese line breaks, a language's fallback font, and the
// mirrored layout and controller of a right-to-left interface.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/input.hpp"
#include "core/save_file.hpp"
#include "orbit/i18n.hpp"
#include "gfx/draw_list.hpp"
#include "gfx/font.hpp"
#include "gfx/font_format.hpp"
#include "gfx/script.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

namespace hui::gfx
{
namespace
{

std::vector<std::uint32_t> codepoints(std::string_view text)
{
    std::vector<std::uint32_t> out;
    for (std::size_t i = 0; i < text.size();)
        out.push_back(next_codepoint(text, &i));
    return out;
}

// A .huifont with one 2x2 glyph per codepoint, each `advance` wide at 10 px.
std::string tiny_font(std::initializer_list<std::uint32_t> glyphs, float advance)
{
    namespace ff = font_format;
    ff::Header header{};
    header.magic = ff::kMagic;
    header.version = ff::kVersion;
    header.atlas_width = 4;
    header.atlas_height = 2;
    header.pixel_size = 10.0f;
    header.sdf_range = 2.0f;
    header.ascent = 8.0f;
    header.descent = -2.0f;
    header.glyph_count = static_cast<std::uint32_t>(glyphs.size());
    std::string out(reinterpret_cast<const char *>(&header), sizeof(header));
    for (const std::uint32_t c : glyphs)
    {
        ff::Glyph glyph{c, 0, 0, 2, 2, 0.0f, -2.0f, advance};
        out.append(reinterpret_cast<const char *>(&glyph), sizeof(glyph));
    }
    out.append(8, '\x80');
    return out;
}

TEST(Script, BundledPrimaryFontsShowEveryLanguageNameBeforeSelection)
{
    for (const char *name : {"inter-regular", "inter-medium", "inter-semibold", "inter-display-light",
                             "inter-tight-extralight", "inter-tight-light", "inter-tight-medium",
                             "inter-tight-semibold", "sora-regular", "sora-semibold"})
    {
        SCOPED_TRACE(name);
        // Read as the app reads it on the console.
        std::string data;
        ASSERT_TRUE(save::read_file(std::string(HUI_SOURCE_DIR) + "/assets/fonts/" + name + ".huifont", &data,
                                    kFontFileLimit));
        Font font;
        ASSERT_TRUE(font.load(data, false));
        for (const auto &language : orbit::i18n::languages())
            for (const auto point : codepoints(shape(language.name)))
                EXPECT_TRUE(font.has_glyph(point)) << language.code << " U+" << std::hex << point;
    }
}

TEST(Script, ArabicLettersJoinAndLamAlefBecomesOneLigature)
{
    // بيت: beh initial, yeh medial, teh final.
    EXPECT_EQ(codepoints(shape("بيت")), (std::vector<std::uint32_t>{0xFE91, 0xFEF4, 0xFE96}));
    // دار: dal does not join forward, so alef is isolated after it.
    EXPECT_EQ(codepoints(shape("دار")), (std::vector<std::uint32_t>{0xFEA9, 0xFE8D, 0xFEAD}));
    // لا alone and after a joining letter (سلام: seen, lam-alef final, meem).
    EXPECT_EQ(codepoints(shape("لا")), (std::vector<std::uint32_t>{0xFEFB}));
    EXPECT_EQ(codepoints(shape("سلام")), (std::vector<std::uint32_t>{0xFEB3, 0xFEFC, 0xFEE1}));
    // Vowel marks are dropped; Latin text is untouched.
    EXPECT_EQ(codepoints(shape("بَ")), (std::vector<std::uint32_t>{0xFE8F}));
    EXPECT_EQ(shape("Orbit Zero"), "Orbit Zero");
}

TEST(Script, RightToLeftTextIsReorderedButLatinAndNumbersKeepTheirOrder)
{
    set_right_to_left(true);
    // "تنزيل" reversed for left-to-right drawing: zain doesn't join forward,
    // so the yeh after it starts a new joined pair.
    const std::string arabic = visual("تنزيل");
    EXPECT_EQ(codepoints(arabic), (std::vector<std::uint32_t>{0xFEDE, 0xFEF3, 0xFEB0, 0xFEE8, 0xFE97}));
    // A Latin name with its version reads left to right, left of the Arabic
    // word it follows (UAX #9: the number joins the Latin run).
    EXPECT_EQ(visual("تنزيل Orbit Zero 1.7.5"), "Orbit Zero 1.7.5 " + arabic);
    // A number between Arabic words keeps its own order.
    EXPECT_EQ(visual("تنزيل 12 تنزيل"), arabic + " 12 " + arabic);
    // Brackets mirror in right-to-left runs.
    EXPECT_EQ(codepoints(visual("(ب)")), (std::vector<std::uint32_t>{'(', 0xFE8F, ')'}));
    EXPECT_EQ(visual("Orbit 1.7.5"), "Orbit 1.7.5");
    set_right_to_left(false);
}

TEST(Script, ChineseAndJapaneseBreakBetweenCharactersButNotBeforeClosingPunctuation)
{
    EXPECT_TRUE(breaks_between(0x3042, 0x3044));  // あ|い
    EXPECT_TRUE(breaks_between('a', 0x4E2D));     // a|中
    EXPECT_FALSE(breaks_between(0x4E2D, 0x3002)); // 中。
    EXPECT_FALSE(breaks_between(0x300C, 0x4E2D)); // 「中
    EXPECT_FALSE(breaks_between('a', 'b'));
    EXPECT_TRUE(breaks_between(' ', 'b'));
}

TEST(Script, MissingCharactersComeFromTheFallbackFont)
{
    Font latin, cjk;
    ASSERT_TRUE(latin.load(tiny_font({'?', 'a'}, 5.0f)));
    ASSERT_TRUE(cjk.load(tiny_font({0x3042, 0x3044}, 10.0f), false));
    ASSERT_NE(cjk.atlas_pixels(), nullptr);
    EXPECT_TRUE(cjk.atlas().empty()); // kept in the loaded data, not copied
    EXPECT_FLOAT_EQ(latin.measure("aあ", 10.0f), 10.0f); // あ is a question mark
    latin.set_fallback(&cjk, 7);
    EXPECT_FLOAT_EQ(latin.measure("aあ", 10.0f), 15.0f);
    std::vector<GlyphQuad> quads;
    latin.layout("aあ", 0.0f, 10.0f, 10.0f, Align::left, quads);
    ASSERT_EQ(quads.size(), 2u);
    EXPECT_FALSE(quads[0].fallback);
    EXPECT_TRUE(quads[1].fallback);
    // Five characters 10 wide in 25: lines break between them.
    const std::vector<std::string> lines = latin.wrap("あいあいあ", 10.0f, 25.0f);
    EXPECT_EQ(lines, (std::vector<std::string>{"あい", "あい", "あ"}));
    cjk.forget_atlas();
    EXPECT_EQ(cjk.atlas_pixels(), nullptr);
}

TEST(Script, ArabicJoiningIgnoresLatinLetterSpacing)
{
    Font font;
    ASSERT_TRUE(font.load(tiny_font({0xFE91, 0xFE96, 0xFEF4}, 10.0f)));
    EXPECT_FLOAT_EQ(font.measure("بيت", 10.0f, 4.0f), font.measure("بيت", 10.0f));
    std::vector<GlyphQuad> tight, tracked;
    font.layout("بيت", 0, 0, 10, Align::left, tight);
    font.layout("بيت", 0, 0, 10, Align::left, tracked, 4);
    ASSERT_EQ(tight.size(), tracked.size());
    for (std::size_t i = 0; i < tight.size(); ++i) EXPECT_FLOAT_EQ(tight[i].x0, tracked[i].x0);
}

TEST(Script, CompressedFontsInflateToTheirExactSize)
{
    const std::string font = tiny_font({0x3042}, 10.0f);
    // "HUFZ", the size, then a zlib stream with one stored deflate block.
    std::string packed = "HUFZ";
    const std::uint32_t size = static_cast<std::uint32_t>(font.size());
    packed.append(reinterpret_cast<const char *>(&size), 4);
    packed += "\x78\x01\x01";
    const std::uint16_t length = static_cast<std::uint16_t>(font.size()), inverse = static_cast<std::uint16_t>(~length);
    packed.append(reinterpret_cast<const char *>(&length), 2);
    packed.append(reinterpret_cast<const char *>(&inverse), 2);
    packed += font;
    packed.append(4, '\0'); // Adler-32, unchecked by stb
    std::size_t inflated = 0;
    const FontBytes bytes = inflate_font(packed, &inflated);
    ASSERT_NE(bytes, nullptr);
    ASSERT_EQ(inflated, font.size());
    EXPECT_EQ(std::memcmp(bytes.get(), font.data(), font.size()), 0);
    // A corrupt size must neither allocate excessively nor inflate beyond it.
    std::string broken = packed;
    std::uint32_t too_big = 0x7fffffffu;
    std::memcpy(broken.data() + 4, &too_big, 4);
    EXPECT_EQ(inflate_font(broken, &inflated), nullptr);
    std::uint32_t too_small = 1;
    std::memcpy(broken.data() + 4, &too_small, 4);
    EXPECT_EQ(inflate_font(broken, &inflated), nullptr);
    EXPECT_EQ(inflate_font("not a font", &inflated), nullptr);
    EXPECT_EQ(inflated, 0u);
}

TEST(Script, BundledLanguageFontsLoadAsTheConsoleReadsThem)
{
    // Each language's font, read as the app reads it on the console: whole,
    // then inflated. "Settings" in each language must draw from it.
    const std::pair<const char *, const char *> languages[] = {
        {"ja", "設定"}, {"zh-Hans", "设置"}, {"zh-Hant", "設定"}, {"ko", "설정"}, {"ar", "الإعدادات"}};
    for (const auto &[code, sample] : languages)
    {
        SCOPED_TRACE(code);
        const std::string file(orbit::i18n::fallback_font(code));
        std::string packed;
        ASSERT_TRUE(save::read_file(std::string(HUI_SOURCE_DIR) + "/assets/fonts/" + file, &packed, kFontFileLimit));
        std::size_t size = 0;
        const FontBytes bytes = inflate_font(packed, &size);
        ASSERT_NE(bytes, nullptr);
        Font font;
        ASSERT_TRUE(font.load(std::string_view(bytes.get(), size), false));
        for (const auto point : codepoints(shape(sample)))
            EXPECT_TRUE(font.has_glyph(point)) << " U+" << std::hex << point;
    }
}

TEST(Script, RightToLeftMirrorsShapesButNotText)
{
    Font font;
    ASSERT_TRUE(font.load(tiny_font({'?', 'a', 'b'}, 10.0f)));
    DrawList list;
    list.set_mirror(kVirtualWidth);
    list.rounded_rect({100.0f, 50.0f, 200.0f, 40.0f}, 0.0f, Color{1, 1, 1, 1});
    ASSERT_EQ(list.instances().size(), 1u);
    EXPECT_FLOAT_EQ(list.instances()[0].rect[0], kVirtualWidth - 300.0f);
    // Left-aligned text at x 100 ends at the mirrored edge, in reading order.
    list.text(font, 1, "ab", 100.0f, 80.0f, 10.0f, Color{1, 1, 1, 1});
    ASSERT_EQ(list.instances().size(), 3u);
    EXPECT_FLOAT_EQ(list.instances()[1].rect[0], kVirtualWidth - 100.0f - 20.0f); // a
    EXPECT_FLOAT_EQ(list.instances()[2].rect[0], kVirtualWidth - 100.0f - 10.0f); // b, right of a
}

TEST(Script, AMirroredControllerSwapsLeftAndRight)
{
    InputFrame input;
    input.nav = Direction::left;
    input.pressed = action_bit(Action::page_prev) | action_bit(Action::confirm);
    input.stick_x = -0.5f;
    const InputFrame out = mirror_horizontal(input);
    EXPECT_EQ(out.nav, Direction::right);
    EXPECT_EQ(out.pressed, action_bit(Action::page_next) | action_bit(Action::confirm));
    EXPECT_FLOAT_EQ(out.stick_x, 0.5f);
}

} // namespace
} // namespace hui::gfx
