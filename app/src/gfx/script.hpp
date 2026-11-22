// ps5-homebrew-ui - Scripts the SDF fonts can't draw in logical order alone:
// Arabic letters take their joined forms, right-to-left text is reordered
// for display, and Chinese and Japanese lines may break between characters.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace hui::gfx
{

std::uint32_t next_codepoint(std::string_view text, std::size_t *index);
void append_utf8(std::string &out, std::uint32_t codepoint);

// The interface reads right to left (Arabic). Text with right-to-left letters
// then forms a right-to-left paragraph; text without any stays as it is.
void set_right_to_left(bool rtl);
bool right_to_left();

// True when the text has a letter that reads right to left.
bool has_right_to_left(std::string_view text);

// Arabic letters as the presentation forms that join them to their
// neighbours (initial, medial, final, isolated, and the lam-alef ligatures).
// Vowel marks are dropped: the fonts have no way to place them. Text without
// Arabic is returned unchanged.
std::string shape(std::string_view text);

// The order glyphs are drawn in, left to right: shaped, with right-to-left
// runs reversed (and their brackets mirrored) and the runs of a right-to-left
// paragraph in reverse order. Latin words and numbers inside keep their order.
std::string visual(std::string_view text);

// Lines may break after this character and before the next one: after a
// space, between Chinese or Japanese characters (not before closing or after
// opening punctuation), as UAX #14 allows for those scripts.
bool breaks_between(std::uint32_t before, std::uint32_t after);

} // namespace hui::gfx
