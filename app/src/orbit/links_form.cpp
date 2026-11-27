// Orbit Store TV app - Download from links (Downloads).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The browser's Download from links (src/AddLinks.tsx) for a controller: a link row
// that Cross types into with the look's keyboard, the drive it goes to, and Download
// and Cancel under them. Opened for a paused or failed link download, it takes a
// fresh link instead and keeps the part already downloaded. Long links are easier
// from a phone or computer, so the form names Orbit's address for them.

#include "orbit/screens.hpp"
#include "orbit/i18n.hpp"
#include "orbit/icons.hpp"
#include "orbit/look.hpp"

#include <algorithm>

namespace orbit
{

namespace
{

constexpr float kLeft = 360.0f;
constexpr float kWidth = 1200.0f;
constexpr float kTop = 200.0f;
constexpr float kRow = 72.0f;
constexpr float kValueX = 240.0f; // where a row's value starts, from its left edge
constexpr std::size_t kLimit = 2047;

void wipe(std::string &text)
{
    std::fill(text.begin(), text.end(), '\0');
    text.clear();
}

// The end of a long link, which tells links apart better than their start.
std::string tail(const std::string &link, std::size_t keep)
{
    return link.size() <= keep ? link : "\xE2\x80\xA6" + link.substr(link.size() - keep);
}

} // namespace

LinksForm::LinksForm(Context &context) : ctx_(context)
{
    keyboard_.style.bindings = hui::ui::KeyboardBindings::standard();
}

void LinksForm::open(std::string replace_job, std::string replace_title)
{
    wipe(link_);
    replace_job_ = std::move(replace_job);
    replace_title_ = std::move(replace_title);
    const Snapshot &state = ctx_.store.state();
    drive_ = std::max(0, default_drive(state.drives, state.system.preferred_storage));
    row_ = 0;
    button_ = 0;
    typing_ = false;
    waiting_ = false;
    serial_ = ctx_.store.setting_result().serial;
    error_.clear();
    note_.clear();
    open_ = true;
    in_.snap(0.0f);
    in_.target = 1.0f;
}

void LinksForm::close()
{
    wipe(link_);
    typing_ = false;
    open_ = false;
}

int LinksForm::button_row() const
{
    return replace_job_.empty() ? 2 : 1;
}

LinksForm::Result LinksForm::update(const InputFrame &input, float dt, Feedback &feedback)
{
    in_.update(dt);
    if (!open_)
        return Result::none;
    // The keyboard of the look the form is in.
    if (look::is(look::Interface::cinema))
    {
        cn::keyboard_look(keyboard_.style);
        keyboard_.set_bounds(cn::keyboard_keys());
    }
    else if (look::is(look::Interface::panorama))
    {
        pano::keyboard_look(keyboard_.style);
        keyboard_.set_bounds(pano::keyboard_keys());
    }
    else
    {
        const bool crossbar = look::is(look::Interface::crossbar);
        xb::keyboard_look(keyboard_.style, crossbar);
        keyboard_.set_bounds(crossbar ? xb::keyboard_keys() : ui::Rect{320.0f, 590.0f, 1280.0f, 400.0f});
    }
    keyboard_.style.bindings = hui::ui::KeyboardBindings::standard();
    keyboard_.style.max_length = static_cast<int>(kLimit);
    keyboard_.style.done_label = tr("Done");
    keyboard_.set_active(typing_);
    keyboard_.update(dt);

    // The check the PS5 has finished.
    const SettingResult &result = ctx_.store.setting_result();
    if (waiting_ && result.serial != serial_ && !result.busy)
    {
        serial_ = result.serial;
        waiting_ = false;
        if (result.error.empty())
        {
            note_ = result.note;
            close();
            feedback.play(hui::audio::Cue::modal_close);
            return Result::added;
        }
        error_ = tr(std::string_view(result.error));
    }

    if (typing_)
    {
        const hui::ui::Event event = keyboard_.handle(input, feedback);
        for (int i = 0; i < keyboard_.erased() && !link_.empty(); ++i)
            link_.pop_back(); // links are ASCII
        std::string typed = keyboard_.typed();
        typed.erase(std::remove(typed.begin(), typed.end(), ' '), typed.end());
        if (link_.size() + typed.size() <= kLimit)
            link_ += typed;
        wipe(typed);
        keyboard_.set_length(static_cast<int>(link_.size()));
        if (event == hui::ui::Event::activated || event == hui::ui::Event::cancelled)
        {
            typing_ = false;
            if (event == hui::ui::Event::activated)
                row_ = button_row();
            feedback.play(hui::audio::Cue::modal_close);
        }
        return Result::none;
    }

    const std::vector<Drive> &drives = ctx_.store.state().drives;
    drive_ = std::clamp(drive_, 0, std::max(0, static_cast<int>(drives.size()) - 1));
    const bool ready = link_.rfind("https://", 0) == 0 && link_.size() > 8 &&
                       (!replace_job_.empty() || !drives.empty());
    const int last = button_row();
    if (input.is_pressed(Action::back))
    {
        if (waiting_)
            refuse(feedback, input);
        else
        {
            close();
            feedback.play(hui::audio::Cue::back);
            return Result::closed;
        }
    }
    else if (input.nav == Direction::up || input.nav == Direction::down)
    {
        const int to = row_ + (input.nav == Direction::down ? 1 : -1);
        if (to >= 0 && to <= last)
        {
            row_ = to;
            feedback.play(hui::audio::Cue::focus);
        }
        else
            refuse(feedback, input);
    }
    else if (input.nav == Direction::left || input.nav == Direction::right)
    {
        const int step = input.nav == Direction::right ? 1 : -1;
        if (row_ == last && button_ + step >= 0 && button_ + step < 2)
        {
            button_ += step;
            feedback.play(hui::audio::Cue::focus);
        }
        else if (row_ == 1 && last == 2 && !waiting_ && drive_ + step >= 0 &&
                 drive_ + step < static_cast<int>(drives.size()))
        {
            drive_ += step;
            feedback.play(hui::audio::Cue::tab);
        }
        else
            refuse(feedback, input);
    }
    else if (input.is_pressed(Action::confirm))
    {
        if (row_ == last && button_ == 1 && !waiting_)
        {
            close();
            feedback.play(hui::audio::Cue::back);
            return Result::closed;
        }
        if (row_ == last)
        {
            if (waiting_ || result.busy || !ready)
                refuse(feedback, input);
            else
            {
                waiting_ = true;
                serial_ = result.serial;
                error_.clear();
                if (replace_job_.empty())
                    ctx_.store.add_links({link_}, drives[static_cast<std::size_t>(drive_)].id);
                else
                    ctx_.store.replace_link(replace_job_, link_);
                feedback.play(hui::audio::Cue::select);
            }
        }
        else if (waiting_)
            refuse(feedback, input);
        else if (row_ == 0)
        {
            typing_ = true;
            keyboard_.enter();
            keyboard_.set_length(static_cast<int>(link_.size()));
            feedback.play(hui::audio::Cue::modal_open);
        }
        else if (!drives.empty())
        {
            // Cross on the drive moves to the next one.
            drive_ = (drive_ + 1) % static_cast<int>(drives.size());
            feedback.play(hui::audio::Cue::tab);
        }
    }
    return Result::none;
}

void LinksForm::draw(Frame &frame) const
{
    // Only while open: update() stops when it closes, so a fade-out would never finish.
    if (!open_)
        return;
    const ui::Type &type = ctx_.type;
    const Snapshot &state = ctx_.store.state();
    hui::gfx::DrawList &overlay = frame.overlay;
    overlay.push_opacity(in_.value);
    overlay.rounded_rect({0.0f, 0.0f, 1920.0f, 1080.0f}, 0.0f, ui::color::night0.with_alpha(0.92f));
    const bool replacing = !replace_job_.empty();
    if (typing_)
    {
        // Typing: the link on its own above the keys, its end in view.
        ui::text_fit(overlay, type.light, replacing ? tr("New link") : tr("Link"), 320.0f, 395.0f, 46.0f, 1280.0f,
                     ui::color::text);
        ui::text_fit(overlay, type.regular, link_.empty() ? std::string("https://") : tail(link_, 72), 320.0f, 520.0f,
                     30.0f, 1280.0f, link_.empty() ? ui::color::text3 : ui::color::text);
        hui::ui::Canvas canvas{overlay, ctx_.fonts, frame.glass_texture, ctx_.clock};
        keyboard_.draw(canvas);
        overlay.pop_opacity();
        return;
    }
    ui::text_fit(overlay, type.light, replacing ? tr("Replace link") : tr("Download from links"), kLeft, kTop + 44.0f,
                 46.0f, kWidth, ui::color::text);
    const std::string intro =
        replacing ? tr("Type a fresh link to the same file. The part already downloaded is kept.")
                  : tr("Type a direct download link. ZIP, 7z and RAR files unpack like games.");
    ui::paragraph(overlay, type.regular, intro, kLeft, kTop + 100.0f, 21.0f, kWidth, 32.0f, ui::color::text2, 2);
    if (replacing && !replace_title_.empty())
        ui::text_fit(overlay, type.regular, replace_title_, kLeft, kTop + 146.0f, 19.0f, kWidth, ui::color::text3);
    const SettingResult &result = ctx_.store.setting_result();
    const std::vector<Drive> &drives = state.drives;
    const bool ready = link_.rfind("https://", 0) == 0 && link_.size() > 8 && (replacing || !drives.empty());
    const int last = button_row();
    float y = kTop + 176.0f;
    for (int i = 0; i < last; ++i)
    {
        const ui::Rect r{kLeft, y, kWidth, kRow - 12.0f};
        ui::focus_ring(overlay, r, 14.0f, i == row_ ? 1.0f : 0.0f);
        ui::surface(overlay, r, 14.0f, i == row_ ? ui::color::field_open : ui::color::field);
        std::string value;
        if (i == 0)
        {
            ui::text_fit(overlay, type.regular, replacing ? tr("New link") : tr("Link"), r.x + 24.0f, r.cy() + 8.0f,
                         20.0f, kValueX - 40.0f, ui::color::text2);
            value = link_.empty() ? std::string("https://") : tail(link_, 64);
            ui::text_fit(overlay, type.medium, value, r.x + kValueX, r.cy() + 8.0f, 22.0f, r.w - kValueX - 24.0f,
                         link_.empty() ? ui::color::text3 : ui::color::text);
        }
        else
        {
            ui::text_fit(overlay, type.regular, tr("Drive"), r.x + 24.0f, r.cy() + 8.0f, 20.0f, kValueX - 40.0f,
                         ui::color::text2);
            if (drives.empty())
                value = tr("Connect a drive first.");
            else
            {
                const Drive &d = drives[static_cast<std::size_t>(std::clamp(drive_, 0, static_cast<int>(drives.size()) - 1))];
                value = i18n::place(d.label) + " \xC2\xB7 " + tr("{size} free", {{"size", format_bytes(d.free_bytes)}});
            }
            ui::text_fit(overlay, type.medium, value, r.x + kValueX, r.cy() + 8.0f, 22.0f, r.w - kValueX - 80.0f,
                         ui::color::text);
            if (drives.size() > 1)
                {
                    // The fonts have no ‹ ›, so the icon set's arrows say Left and Right switch it.
                    icons::draw(overlay, icons::Icon::back, r.x + r.w - 62.0f, r.cy(), 20.0f, ui::color::text2, 2.0f);
                    icons::draw(overlay, icons::Icon::next, r.x + r.w - 34.0f, r.cy(), 20.0f, ui::color::text2, 2.0f);
                }
        }
        y += kRow;
    }
    y += 12.0f;
    const std::string labels[] = {waiting_ ? tr("Checking\xE2\x80\xA6") : replacing ? tr("Replace link") : tr("Download"),
                                  tr("Cancel")};
    float x = kLeft;
    for (int b = 0; b < 2; ++b)
    {
        const ui::ButtonKind kind = b == 0 ? ui::ButtonKind::primary : ui::ButtonKind::secondary;
        const float w = ui::button_width(type, labels[b], 21.0f, kind);
        const bool enabled = b == 1 ? !waiting_ : !waiting_ && !result.busy && ready;
        ui::button(overlay, type, {x, y, w, ui::kButtonHeight}, labels[b], kind,
                   row_ == last && b == button_ ? 1.0f : 0.0f, enabled, 21.0f);
        x += w + 16.0f;
    }
    y += ui::kButtonHeight + 22.0f;
    if (!error_.empty())
    {
        ui::paragraph(overlay, type.regular, error_, kLeft, y + 14.0f, 21.0f, kWidth, 30.0f, ui::color::bad, 2);
        y += 70.0f;
    }
    if (!state.address.empty())
        ui::paragraph(overlay, type.regular,
                      tr("Long links are easier from a phone or computer: open {address} there and choose Download "
                         "from links in Downloads.",
                         {{"address", "http://" + state.address + ":34177"}}),
                      kLeft, y + 10.0f, 18.0f, kWidth, 28.0f, ui::color::text3, 2);
    overlay.pop_opacity();
}

std::vector<hui::ui::Hint> LinksForm::hints() const
{
    if (typing_)
        return {{hui::ui::Button::cross, tr("Select")}, {hui::ui::Button::circle, tr("Back")}};
    return {{hui::ui::Button::cross, tr("Choose")}, {hui::ui::Button::circle, tr("Cancel")}};
}

} // namespace orbit
