// ps5-homebrew-ui - Arabic joining, right-to-left display order and line
// breaks for Chinese and Japanese (see script.hpp).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gfx/script.hpp"

#include <algorithm>
#include <vector>

namespace hui::gfx
{

namespace
{

bool interface_rtl = false;

// Arabic Presentation Forms-B for U+0621..U+064A: isolated, final, initial,
// medial. 0 where a letter has no such form (it joins on one side only).
struct Forms
{
    std::uint16_t isolated, final, initial, medial;
};
constexpr Forms kForms[] = {
    {0xFE80, 0, 0, 0},                // 0621 hamza
    {0xFE81, 0xFE82, 0, 0},           // 0622 alef with madda above
    {0xFE83, 0xFE84, 0, 0},           // 0623 alef with hamza above
    {0xFE85, 0xFE86, 0, 0},           // 0624 waw with hamza above
    {0xFE87, 0xFE88, 0, 0},           // 0625 alef with hamza below
    {0xFE89, 0xFE8A, 0xFE8B, 0xFE8C}, // 0626 yeh with hamza above
    {0xFE8D, 0xFE8E, 0, 0},           // 0627 alef
    {0xFE8F, 0xFE90, 0xFE91, 0xFE92}, // 0628 beh
    {0xFE93, 0xFE94, 0, 0},           // 0629 teh marbuta
    {0xFE95, 0xFE96, 0xFE97, 0xFE98}, // 062A teh
    {0xFE99, 0xFE9A, 0xFE9B, 0xFE9C}, // 062B theh
    {0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0}, // 062C jeem
    {0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4}, // 062D hah
    {0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8}, // 062E khah
    {0xFEA9, 0xFEAA, 0, 0},           // 062F dal
    {0xFEAB, 0xFEAC, 0, 0},           // 0630 thal
    {0xFEAD, 0xFEAE, 0, 0},           // 0631 reh
    {0xFEAF, 0xFEB0, 0, 0},           // 0632 zain
    {0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4}, // 0633 seen
    {0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8}, // 0634 sheen
    {0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC}, // 0635 sad
    {0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0}, // 0636 dad
    {0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4}, // 0637 tah
    {0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8}, // 0638 zah
    {0xFEC9, 0xFECA, 0xFECB, 0xFECC}, // 0639 ain
    {0xFECD, 0xFECE, 0xFECF, 0xFED0}, // 063A ghain
    {0, 0, 0, 0},                     // 063B..063F: not used in Arabic text
    {0, 0, 0, 0},
    {0, 0, 0, 0},
    {0, 0, 0, 0},
    {0, 0, 0, 0},
    {0, 0, 0, 0},                     // 0640 tatweel: joins, keeps its form
    {0xFED1, 0xFED2, 0xFED3, 0xFED4}, // 0641 feh
    {0xFED5, 0xFED6, 0xFED7, 0xFED8}, // 0642 qaf
    {0xFED9, 0xFEDA, 0xFEDB, 0xFEDC}, // 0643 kaf
    {0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0}, // 0644 lam
    {0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4}, // 0645 meem
    {0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8}, // 0646 noon
    {0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC}, // 0647 heh
    {0xFEED, 0xFEEE, 0, 0},           // 0648 waw
    {0xFEEF, 0xFEF0, 0, 0},           // 0649 alef maksura
    {0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4}, // 064A yeh
};

bool arabic_letter(std::uint32_t c)
{
    return c >= 0x0621 && c <= 0x064A;
}

// Vowel marks and other combining marks: transparent to joining.
bool mark(std::uint32_t c)
{
    return (c >= 0x064B && c <= 0x065F) || c == 0x0670 || (c >= 0x06D6 && c <= 0x06ED);
}

// Joins to the letter after it (dual-joining letters and tatweel).
bool joins_forward(std::uint32_t c)
{
    if (c == 0x0640)
        return true;
    return arabic_letter(c) && kForms[c - 0x0621].initial != 0;
}

// Joins to the letter before it.
bool joins_backward(std::uint32_t c)
{
    if (c == 0x0640)
        return true;
    return arabic_letter(c) && kForms[c - 0x0621].final != 0;
}

// Lam followed by these alefs becomes one ligature: isolated, final.
std::uint32_t lam_alef(std::uint32_t alef, bool final)
{
    switch (alef)
    {
    case 0x0622:
        return final ? 0xFEF6 : 0xFEF5;
    case 0x0623:
        return final ? 0xFEF8 : 0xFEF7;
    case 0x0625:
        return final ? 0xFEFA : 0xFEF9;
    case 0x0627:
        return final ? 0xFEFC : 0xFEFB;
    default:
        return 0;
    }
}

bool rtl_letter(std::uint32_t c)
{
    return (c >= 0x0590 && c <= 0x08FF) || (c >= 0xFB1D && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF);
}

bool digit(std::uint32_t c)
{
    return (c >= '0' && c <= '9') || (c >= 0x0660 && c <= 0x0669) || (c >= 0x06F0 && c <= 0x06F9);
}

bool strong_ltr(std::uint32_t c)
{
    if (c < 0x80)
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    if (rtl_letter(c) || digit(c))
        return false;
    // Letters of the other scripts (Latin, Greek, Cyrillic, CJK, kana, Hangul);
    // punctuation and symbols are neutral.
    return (c >= 0x00C0 && c <= 0x024F && c != 0x00D7 && c != 0x00F7) || (c >= 0x0370 && c <= 0x052F) ||
           (c >= 0x1E00 && c <= 0x1FFF) || (c >= 0x3040 && c <= 0x30FF) || (c >= 0x3400 && c <= 0x9FFF) ||
           (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF);
}

// Brackets swap in right-to-left runs, as the Unicode mirroring property has it.
std::uint32_t mirrored(std::uint32_t c)
{
    switch (c)
    {
    case '(': return ')';
    case ')': return '(';
    case '[': return ']';
    case ']': return '[';
    case '{': return '}';
    case '}': return '{';
    case '<': return '>';
    case '>': return '<';
    case 0x00AB: return 0x00BB; // « »
    case 0x00BB: return 0x00AB;
    default: return c;
    }
}

std::vector<std::uint32_t> decode(std::string_view text)
{
    std::vector<std::uint32_t> out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();)
        out.push_back(next_codepoint(text, &i));
    return out;
}

// Opening punctuation a line must not end with, and closing punctuation (and
// small kana, the prolonged sound mark) a line must not start with.
bool opening(std::uint32_t c)
{
    return c == 0x3008 || c == 0x300A || c == 0x300C || c == 0x300E || c == 0x3010 || c == 0xFF08 ||
           c == 0x201C || c == 0x2018 || c == '(' || c == '[';
}

bool closing(std::uint32_t c)
{
    return c == 0x3001 || c == 0x3002 || c == 0x3009 || c == 0x300B || c == 0x300D || c == 0x300F ||
           c == 0x3011 || c == 0xFF09 || c == 0xFF0C || c == 0xFF0E || c == 0xFF1A || c == 0xFF1B ||
           c == 0xFF01 || c == 0xFF1F || c == 0x30FC || c == 0x30FB || c == 0x2026 || c == 0x201D ||
           c == 0x2019 || c == ',' || c == '.' || c == ':' || c == ';' || c == '!' || c == '?' || c == ')' ||
           c == ']' || c == '%' || (c >= 0x3041 && c <= 0x3049 && c % 2 == 1) || c == 0x3063 ||
           c == 0x3083 || c == 0x3085 || c == 0x3087 || c == 0x30C3 || c == 0x30E3 || c == 0x30E5 ||
           c == 0x30E7 || (c >= 0x30A1 && c <= 0x30A9 && c % 2 == 1);
}

// Chinese and Japanese characters (and their punctuation) break on either side.
bool ideographic(std::uint32_t c)
{
    return (c >= 0x3000 && c <= 0x30FF) || (c >= 0x3400 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) ||
           (c >= 0xFF00 && c <= 0xFF60);
}

} // namespace

void append_utf8(std::string &out, std::uint32_t c)
{
    if (c < 0x80)
        out += static_cast<char>(c);
    else if (c < 0x800)
    {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
    else if (c < 0x10000)
    {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
    else
    {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

void set_right_to_left(bool rtl)
{
    interface_rtl = rtl;
}

bool right_to_left()
{
    return interface_rtl;
}

bool has_right_to_left(std::string_view text)
{
    // Arabic and Hebrew start with lead bytes D6..DF, presentation forms with EF.
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const auto byte = static_cast<unsigned char>(text[i]);
        if (byte >= 0xD6 && byte <= 0xDF)
            return true;
        if (byte == 0xEF)
        {
            std::size_t at = i;
            if (rtl_letter(next_codepoint(text, &at)))
                return true;
        }
    }
    return false;
}

std::string shape(std::string_view text)
{
    if (!has_right_to_left(text))
        return std::string(text);
    std::vector<std::uint32_t> in = decode(text);
    in.erase(std::remove_if(in.begin(), in.end(), [](std::uint32_t c) { return mark(c) || c == 0x200E || c == 0x200F; }),
             in.end());
    std::string out;
    out.reserve(text.size() + 8);
    for (std::size_t i = 0; i < in.size(); ++i)
    {
        const std::uint32_t c = in[i];
        if (!arabic_letter(c) && c != 0x0640)
        {
            append_utf8(out, c);
            continue;
        }
        const bool after = i > 0 && joins_forward(in[i - 1]);
        // Lam-alef: one ligature in place of both letters.
        if (c == 0x0644 && i + 1 < in.size())
        {
            if (const std::uint32_t ligature = lam_alef(in[i + 1], after))
            {
                append_utf8(out, ligature);
                ++i;
                continue;
            }
        }
        if (c == 0x0640)
        {
            append_utf8(out, c);
            continue;
        }
        const bool before = i + 1 < in.size() && joins_backward(in[i + 1]) && joins_forward(c);
        const bool joined = after && joins_backward(c);
        const Forms &forms = kForms[c - 0x0621];
        const std::uint16_t form = joined && before ? forms.medial
                                   : joined          ? forms.final
                                   : before          ? forms.initial
                                                     : forms.isolated;
        append_utf8(out, form != 0 ? form : c);
    }
    return out;
}

std::string visual(std::string_view text)
{
    if (!has_right_to_left(text))
        return std::string(text);
    const std::vector<std::uint32_t> in = decode(shape(text));
    // Resolve each character to right-to-left or not. Digits read left to
    // right; neutrals take the direction on both sides when it agrees, and
    // the paragraph's otherwise.
    const bool paragraph_rtl = interface_rtl || [&] {
        for (const std::uint32_t c : in)
        {
            if (rtl_letter(c))
                return true;
            if (strong_ltr(c))
                return false;
        }
        return false;
    }();
    enum Kind : std::uint8_t { rtl, ltr, neutral };
    std::vector<Kind> kind(in.size());
    for (std::size_t i = 0; i < in.size(); ++i)
        kind[i] = rtl_letter(in[i]) ? rtl : strong_ltr(in[i]) || digit(in[i]) ? ltr : neutral;
    // Separators inside a number stay with it: 1,234.5 and 12:30.
    for (std::size_t i = 1; i + 1 < in.size(); ++i)
    {
        if (kind[i] == neutral && digit(in[i - 1]) && digit(in[i + 1]) &&
            (in[i] == ',' || in[i] == '.' || in[i] == ':' || in[i] == '/'))
            kind[i] = ltr;
    }
    for (std::size_t i = 0; i < in.size();)
    {
        if (kind[i] != neutral)
        {
            ++i;
            continue;
        }
        std::size_t end = i;
        while (end < in.size() && kind[end] == neutral)
            ++end;
        const Kind before = i > 0 ? kind[i - 1] : (paragraph_rtl ? rtl : ltr);
        const Kind after = end < in.size() ? kind[end] : (paragraph_rtl ? rtl : ltr);
        const Kind resolved = before == after ? before : (paragraph_rtl ? rtl : ltr);
        std::fill(kind.begin() + static_cast<std::ptrdiff_t>(i), kind.begin() + static_cast<std::ptrdiff_t>(end),
                  resolved);
        i = end;
    }
    // Runs in logical order, then in display order.
    struct Run
    {
        std::size_t start, end;
        bool rtl;
    };
    std::vector<Run> runs;
    for (std::size_t i = 0; i < in.size();)
    {
        std::size_t end = i;
        while (end < in.size() && kind[end] == kind[i])
            ++end;
        runs.push_back({i, end, kind[i] == rtl});
        i = end;
    }
    if (paragraph_rtl)
        std::reverse(runs.begin(), runs.end());
    std::string out;
    out.reserve(text.size() + 8);
    for (const Run &run : runs)
    {
        if (run.rtl)
        {
            for (std::size_t i = run.end; i > run.start; --i)
                append_utf8(out, mirrored(in[i - 1]));
        }
        else
        {
            for (std::size_t i = run.start; i < run.end; ++i)
                append_utf8(out, in[i]);
        }
    }
    return out;
}

bool breaks_between(std::uint32_t before, std::uint32_t after)
{
    if (before == ' ')
        return true;
    if (closing(after) || opening(before))
        return false;
    return ideographic(before) || ideographic(after);
}

} // namespace hui::gfx
