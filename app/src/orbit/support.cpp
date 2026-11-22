#include "orbit/screens.hpp"
#include "orbit/i18n.hpp"
#include <algorithm>
#include <cmath>
namespace orbit {
std::vector<SupportLink> default_support_links() { return {{"Support with crypto", "https://github.com/saawant12/orbit-store-ps5/issues/131#user-content-support-with-crypto", {"11111110001011111001101100101111101111111","10000010011010010010101010010100101000001","10111010110101111000100010101111001011101","10111010101100000011111010000000101011101","10111010100100010001100100100011101011101","10000010111111010010101001011100101000001","11111110101010101010101010101010101111111","00000000101010011000111100011000100000000","10111110011100101100001111010101001111100","10110101011011101011101111001011111110111","10100111110001001110110000111110111100010","11110000100101001001111000111001100011010","00001111110000010110001101110101000001111","10101000101001110010111010111100001110001","10110110001011110101010000100100110000110","01100001110010000001110010001001101010001","00100111101111001101001001000101010100110","01111001100001100111010101001011101110001","11100111011011111100010001111010110011010","11111001000110001110111001011000111010000","11010011111100010000010010110011000100110","00010101000101110111111111001101011111111","11011111000100110100100001110010000001000","11011000101101011010111000000000100011010","00101010110110101100101011111100000100111","11000101000011000101110100001001001111011","10000110110010011100100000010000110110000","00010000011111001000011100010011110111010","10010011110000101110101111101101000101100","10010100101010000100001001110111101111101","10001010110000000010101110111100011101110","10101001000001011000011100110010111101000","10101111000100010100101101110100111110110","00000000110111101001101100101100100010001","11111110000001101010001000010101101011110","10000010100011001001000110111110100011010","10111010101010100010111000011000111111100","10111010110011111101110110101010110100000","10111010110010000010110001011000011111100","10000010011100101011011110101011111011010","11111110101011110000001011100101000111100"}}}; }
void SupportView::update(const InputFrame &input, float, Feedback &feedback) {
    const auto &links = ctx_.store.state().support_links;
    selected_ = std::clamp(selected_, 0, std::max(0, static_cast<int>(links.size()) - 1));
    if (input.is_pressed(Action::back)) { open_ = false; feedback.play(hui::audio::Cue::modal_close); }
    else if (input.nav == Direction::up || input.nav == Direction::left || input.nav == Direction::down || input.nav == Direction::right) {
        const int next = selected_ + ((input.nav == Direction::up || input.nav == Direction::left) ? -1 : 1);
        if (next >= 0 && next < static_cast<int>(links.size())) { selected_ = next; error_.clear(); feedback.play(hui::audio::Cue::focus); }
    } else if (input.is_pressed(Action::confirm) && !links.empty()) {
        if (!ctx_.open_support || !ctx_.open_support(links[selected_].url)) error_ = tr("The browser could not be opened.");
        else error_.clear();
    }
}
void SupportView::draw(Frame &frame) const {
    if (!open_) return;
    const auto &links = ctx_.store.state().support_links;
    if (links.empty()) return;
    const int selected = std::clamp(selected_, 0, static_cast<int>(links.size()) - 1);
    const auto &link = links[selected]; const auto &type = ctx_.type; auto &list = frame.overlay;
    list.rounded_rect({0, 0, 1920, 1080}, 0, ui::color::night0.with_alpha(.98f));
    ui::text(list, type.light, tr("Support Orbit"), 240, 190, 48, ui::color::text);
    ui::text(list, type.medium, tr("Orbit stays free."), 240, 300, 30, ui::color::text);
    ui::paragraph(list, type.regular, tr("If Orbit is useful to you, you can help cover the costs of AI tools, test consoles, hardware and servers."), 240, 360, 24, 760, 38, ui::color::text2, 4);
    ui::text(list, type.regular, tr("Support is entirely optional."), 240, 510, 21, ui::color::text3);
    // A viewport of four providers keeps the selected funding destination readable on TV.
    const int first = std::max(0, selected - 3);
    // Light buttons keep one width: a focused primary button grows, which made the selected one look wider.
    for (int i = first; i < std::min(first + 4, static_cast<int>(links.size())); ++i) {
        const float y = 558 + (i - first) * 78.0f;
        ui::button(list, type, {240, y, 760, 58}, links[i].label, ui::ButtonKind::light, i == selected ? 1.f : 0.f, true);
    }
    const float module = std::floor(400.f / (link.qr.size()+8));
    const float square = module * (link.qr.size()+8), left = 1190 + (400-square)/2;
    list.rounded_rect({left, 290, square, square}, 0, ui::Color{1,1,1,1});
    for (std::size_t y = 0; y < link.qr.size(); ++y) for (std::size_t x = 0; x < link.qr[y].size(); ++x)
        if (link.qr[y][x] == '1') list.rounded_rect({left+(x+4)*module, 290+(y+4)*module, module, module}, 0, ui::Color{0,0,0,1});
    ui::text_fit(list, type.regular, tr("Scan with your phone"), 1190, 750, 23, 420, ui::color::text);
    ui::paragraph(list, type.regular, link.url, 1190, 795, 18, 420, 27, ui::color::text2, 4);
    if (!error_.empty()) ui::text_fit(list, type.regular, error_, 240, 910, 22, 1000, ui::color::text2);
}
std::vector<hui::ui::Hint> SupportView::hints() const {
    return {{hui::ui::Button::dpad, tr("Support options")}, {hui::ui::Button::cross, tr("Open support page")}, {hui::ui::Button::circle, tr("Back")}};
}
}
