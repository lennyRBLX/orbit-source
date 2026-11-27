// Orbit Store TV app - Free up space (game page, Library, Downloads).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The browser's Free up space (src/FreeSpace.tsx) for a controller. A download that
// doesn't fit first shows how much is missing, with other drives where it fits. The
// list shows what Orbit can delete on the drive, biggest first: Orbit's leftovers
// (ticked) above the games ShadowMount can delete (never ticked). Cross ticks a row;
// the action asks once, then the console deletes one item at a time while the form
// follows it. Once there's room the caller starts the download or the unpack.

#include "orbit/screens.hpp"
#include "orbit/i18n.hpp"
#include "orbit/icons.hpp"

#include <algorithm>

namespace orbit
{

namespace
{

constexpr float kLeft = 360.0f;
constexpr float kWidth = 1200.0f;
constexpr float kTop = 150.0f;
constexpr float kRow = 70.0f;       // a list row and its gap
constexpr float kHeading = 48.0f;   // a group's heading
constexpr float kListEnd = 812.0f;  // where the list stops, above the message
constexpr float kButtons = 870.0f;  // the list's and the confirmation's buttons
constexpr std::size_t kOthers = 2;  // other drives offered on Not enough space

bool has(const std::vector<std::string> &ids, const std::string &id)
{
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

const char *kind_label(const std::string &kind)
{
    if (kind == "package")
        return tr("Installed package");
    if (kind == "archive")
        return tr("Archive kept after unpacking");
    return tr("Cancelled download");
}

std::string item_sub(const SpaceItem &item)
{
    std::string sub;
    const auto add = [&sub](std::string_view part)
    {
        if (!part.empty())
            sub += (sub.empty() ? "" : " \xC2\xB7 ") + std::string(part);
    };
    if (item.kind == "game")
    {
        add(item.format);
        if (item.mounted)
            add(tr("Mounted"));
    }
    else
    {
        add(kind_label(item.kind));
        add(item.name);
    }
    return sub;
}

} // namespace

FreeSpaceForm::FreeSpaceForm(Context &context) : ctx_(context)
{
}

void FreeSpaceForm::open(Request request)
{
    request_ = std::move(request);
    storage_ = request_.storage_id;
    chosen_drive_.clear();
    step_ = request_.need_bytes > 0.0 ? Step::shortfall : Step::list;
    selected_.clear();
    row_ = 0;
    button_ = 0;
    waiting_ = 0;
    listed_ = false;
    error_.clear();
    notice_.clear();
    done_ = {};
    open_ = true;
    in_.snap(0.0f);
    in_.target = 1.0f;
    scroll_.snap(0.0f);
    load(false);
    if (step_ == Step::list)
        row_ = static_cast<int>(rows().size()) - 1;
}

void FreeSpaceForm::load(bool keep)
{
    keep_ = keep;
    waiting_ = waiting_ == 2 ? 2 : 1;
    serial_ = ctx_.store.setting_result().serial;
    ctx_.store.list_space(storage_);
}

void FreeSpaceForm::close(Feedback &feedback, hui::audio::Cue cue)
{
    // The deletion goes on on the console; this only stops following it.
    ctx_.store.want_space(false);
    open_ = false;
    feedback.play(cue);
}

const SpaceListing *FreeSpaceForm::listing() const
{
    const Snapshot &state = ctx_.store.state();
    return listed_ && state.have_space && state.space.storage_id == storage_ ? &state.space : nullptr;
}

double FreeSpaceForm::projected_free() const
{
    if (const SpaceListing *list = listing())
        return list->projected_free_bytes;
    for (const Drive &d : ctx_.store.state().drives)
        if (d.id == storage_)
            return d.projected_free_bytes;
    return 0.0;
}

double FreeSpaceForm::shortfall() const
{
    return request_.need_bytes > 0.0 ? std::max(0.0, request_.need_bytes - projected_free()) : 0.0;
}

std::vector<const SpaceItem *> FreeSpaceForm::chosen_items() const
{
    std::vector<const SpaceItem *> out;
    if (const SpaceListing *list = listing())
    {
        for (const SpaceItem &item : list->leftovers)
            if (has(selected_, item.id))
                out.push_back(&item);
        for (const SpaceItem &item : list->games)
            if (has(selected_, item.id))
                out.push_back(&item);
    }
    return out;
}

double FreeSpaceForm::chosen() const
{
    double sum = 0.0;
    for (const SpaceItem *item : chosen_items())
        sum += item->size_bytes;
    return sum;
}

std::vector<FreeSpaceForm::Row> FreeSpaceForm::rows() const
{
    std::vector<Row> out;
    if (step_ == Step::list)
    {
        if (request_.need_bytes <= 0.0 && ctx_.store.state().drives.size() > 1)
            out.push_back({0});
        if (const SpaceListing *list = listing())
        {
            for (const SpaceItem &item : list->leftovers)
                out.push_back({1, &item});
            for (const SpaceItem &item : list->games)
                out.push_back({1, &item});
        }
    }
    out.push_back({2});
    return out;
}

float FreeSpaceForm::row_top(int index) const
{
    const SpaceListing *list = listing();
    if (list == nullptr)
        return 0.0f;
    const int leftovers = static_cast<int>(list->leftovers.size());
    float y = 0.0f;
    if (leftovers > 0)
        y += kHeading;
    if (index < leftovers)
        return y + kRow * static_cast<float>(index);
    y += kRow * static_cast<float>(leftovers);
    if (!list->games.empty())
        y += kHeading;
    return y + kRow * static_cast<float>(index - leftovers);
}

std::vector<std::string> FreeSpaceForm::buttons() const
{
    const std::string action = request_.then == Then::resume     ? tr("Delete and resume")
                               : request_.then == Then::download ? tr("Delete and download")
                                                                 : tr("Delete");
    switch (step_)
    {
    case Step::shortfall:
    {
        std::vector<std::string> out{tr("Free up space")};
        const std::vector<Drive> &drives = ctx_.store.state().drives;
        for (std::size_t i = 0; i < request_.others.size() && i < kOthers; ++i)
            for (const Drive &d : drives)
                if (d.id == request_.others[i])
                    out.push_back(tr("Use {drive} ({free} free)", {{"drive", i18n::place(d.label)},
                                                                   {"free", format_bytes(d.projected_free_bytes)}}));
        out.push_back(tr("Cancel"));
        return out;
    }
    case Step::list:
        return {action, tr("Cancel")};
    case Step::confirm:
        return {waiting_ == 3 ? std::string(tr("Deleting\xE2\x80\xA6")) : action, tr("Back")};
    case Step::deleting:
        break;
    }
    return {tr("Close")};
}

void FreeSpaceForm::finish(const SpaceDeletion &done)
{
    step_ = Step::list;
    row_ = static_cast<int>(rows().size()) - 1;
    button_ = 0;
    if (done.state == "failed")
        error_ = done.message.empty() ? std::string(tr("An item couldn't be deleted. It was kept."))
                                      : std::string(tr(std::string_view(done.message)));
    else if (request_.then == Then::none)
        notice_ = tr("Deleted {size}.", {{"size", format_bytes(done.freed_bytes)}});
    else if (shortfall() > 0.0)
        error_ = tr("Still {size} short. Select more to delete.", {{"size", format_bytes(shortfall())}});
}

FreeSpaceForm::Result FreeSpaceForm::update(const InputFrame &input, float dt, Feedback &feedback)
{
    in_.update(dt);
    scroll_.update(dt);
    if (!open_)
        return Result::none;
    const Snapshot &state = ctx_.store.state();
    const SettingResult &result = ctx_.store.setting_result();

    // What the console answered.
    if (waiting_ != 0 && result.serial != serial_ && !result.busy)
    {
        serial_ = result.serial;
        const int was = waiting_;
        waiting_ = 0;
        if (!result.error.empty())
        {
            error_ = tr(std::string_view(result.error));
            if (was == 3)
            {
                // Refused (something changed): back to a fresh list, the ticks kept.
                step_ = Step::list;
                load(true);
            }
        }
        else if (was == 3)
        {
            step_ = Step::deleting;
            ctx_.store.want_space(true);
        }
        else if (!state.have_space || state.space.storage_id != storage_)
            load(keep_); // another answer arrived first
        else
        {
            listed_ = true;
            const SpaceListing &list = state.space;
            std::vector<std::string> ids;
            for (const SpaceItem &item : list.leftovers)
                if (!keep_ || has(selected_, item.id))
                    ids.push_back(item.id);
            if (keep_)
                for (const SpaceItem &item : list.games)
                    if (has(selected_, item.id))
                        ids.push_back(item.id);
            selected_ = std::move(ids);
            if (was == 2)
            {
                const bool fits = request_.then != Then::none && done_.state != "failed" && shortfall() <= 0.0;
                finish(done_);
                if (fits)
                {
                    close(feedback, hui::audio::Cue::modal_close);
                    return Result::freed;
                }
            }
            else if (list.deletion.state == "deleting" && list.deletion.storage_id == storage_)
            {
                // Another device is deleting on this drive: follow it here too.
                step_ = Step::deleting;
                ctx_.store.want_space(true);
            }
            const int last = static_cast<int>(rows().size()) - 1;
            row_ = std::min(row_, last);
        }
    }
    // The deletion ended: read the drive again, then decide.
    if (step_ == Step::deleting && waiting_ == 0 && state.space.deletion.state != "deleting")
    {
        done_ = state.space.deletion;
        ctx_.store.want_space(false);
        waiting_ = 2;
        load(false);
    }

    const std::vector<Row> list_rows = rows();
    const int last = static_cast<int>(list_rows.size()) - 1;
    row_ = std::clamp(row_, 0, last);
    const std::vector<std::string> labels = buttons();
    button_ = std::clamp(button_, 0, static_cast<int>(labels.size()) - 1);
    const Row &here = list_rows[static_cast<std::size_t>(row_)];
    const std::vector<Drive> &drives = state.drives;
    const auto next_drive = [&](int step)
    {
        if (drives.size() < 2 || waiting_ != 0)
            return false;
        int at = 0;
        for (std::size_t i = 0; i < drives.size(); ++i)
            if (drives[i].id == storage_)
                at = static_cast<int>(i);
        const int n = static_cast<int>(drives.size());
        storage_ = drives[static_cast<std::size_t>(((at + step) % n + n) % n)].id;
        listed_ = false;
        selected_.clear();
        notice_.clear();
        error_.clear();
        load(false);
        return true;
    };

    if (input.is_pressed(Action::back))
    {
        if (step_ == Step::confirm)
        {
            if (waiting_ == 3)
                refuse(feedback, input);
            else
            {
                step_ = Step::list;
                error_.clear();
                feedback.play(hui::audio::Cue::back);
            }
        }
        else
        {
            close(feedback, hui::audio::Cue::back);
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
        if (here.kind == 2 && button_ + step >= 0 && button_ + step < static_cast<int>(labels.size()))
        {
            button_ += step;
            feedback.play(hui::audio::Cue::focus);
        }
        else if (here.kind == 0 && next_drive(step))
            feedback.play(hui::audio::Cue::tab);
        else
            refuse(feedback, input);
    }
    else if (input.is_pressed(Action::confirm))
    {
        if (here.kind == 0)
        {
            if (next_drive(1))
                feedback.play(hui::audio::Cue::tab);
            else
                refuse(feedback, input);
        }
        else if (here.kind == 1)
        {
            const std::string &id = here.item->id;
            const auto at = std::find(selected_.begin(), selected_.end(), id);
            if (at == selected_.end())
                selected_.push_back(id);
            else
                selected_.erase(at);
            notice_.clear();
            feedback.play(hui::audio::Cue::select);
        }
        else if (step_ == Step::shortfall)
        {
            if (button_ == 0)
            {
                step_ = Step::list;
                row_ = static_cast<int>(rows().size()) - 1;
                button_ = 0;
                feedback.play(hui::audio::Cue::select);
            }
            else if (button_ == static_cast<int>(labels.size()) - 1)
            {
                close(feedback, hui::audio::Cue::back);
                return Result::closed;
            }
            else
            {
                chosen_drive_ = request_.others[static_cast<std::size_t>(button_ - 1)];
                close(feedback, hui::audio::Cue::select);
                return Result::use_drive;
            }
        }
        else if (step_ == Step::list)
        {
            if (button_ == 1)
            {
                close(feedback, hui::audio::Cue::back);
                return Result::closed;
            }
            const std::vector<const SpaceItem *> items = chosen_items();
            if (waiting_ != 0 || items.empty() || chosen() < shortfall())
                refuse(feedback, input);
            else
            {
                step_ = Step::confirm;
                button_ = 0;
                error_.clear();
                notice_.clear();
                feedback.play(hui::audio::Cue::modal_open);
            }
        }
        else if (step_ == Step::confirm)
        {
            if (button_ == 1)
            {
                if (waiting_ == 3)
                    refuse(feedback, input);
                else
                {
                    step_ = Step::list;
                    feedback.play(hui::audio::Cue::back);
                }
            }
            else if (waiting_ != 0 || result.busy)
                refuse(feedback, input);
            else
            {
                std::vector<std::string> ids;
                for (const SpaceItem *item : chosen_items())
                    ids.push_back(item->id);
                waiting_ = 3;
                serial_ = result.serial;
                error_.clear();
                ctx_.store.delete_space(storage_, ids);
                feedback.play(hui::audio::Cue::select);
            }
        }
        else
        {
            close(feedback, hui::audio::Cue::back);
            return Result::closed;
        }
    }

    // Keep the focused row in view.
    if (step_ == Step::list && here.kind == 1)
    {
        const int first = list_rows.front().kind == 0 ? 1 : 0;
        const float top = row_top(row_ - first);
        const float area = kListEnd - (kTop + 200.0f);
        float target = scroll_.target;
        if (top - kHeading < target)
            target = std::max(0.0f, top - kHeading);
        else if (top + kRow > target + area)
            target = top + kRow - area;
        scroll_.target = target;
    }
    else if (step_ == Step::list && here.kind == 0)
        scroll_.target = 0.0f;
    return Result::none;
}

void FreeSpaceForm::draw(Frame &frame) const
{
    // Only while open: update() stops when it closes, so a fade-out would never finish.
    if (!open_)
        return;
    const ui::Type &type = ctx_.type;
    const Snapshot &state = ctx_.store.state();
    hui::gfx::DrawList &overlay = frame.overlay;
    overlay.push_opacity(in_.value);
    overlay.rounded_rect({0.0f, 0.0f, 1920.0f, 1080.0f}, 0.0f, ui::color::night0.with_alpha(0.94f));
    ui::text_fit(overlay, type.light, step_ == Step::shortfall ? tr("Not enough space") : tr("Free up space"), kLeft,
                 kTop + 44.0f, 46.0f, kWidth, ui::color::text);

    const SpaceListing *list = listing();
    const Drive *drive = nullptr;
    for (const Drive &d : state.drives)
        if (d.id == storage_)
            drive = &d;
    const std::string drive_name = i18n::place(list != nullptr ? list->label : drive != nullptr ? drive->label : "");
    const std::vector<Row> list_rows = rows();
    const std::vector<std::string> labels = buttons();
    const int focused_button = list_rows[static_cast<std::size_t>(std::clamp(row_, 0, static_cast<int>(list_rows.size()) - 1))].kind == 2
                                   ? button_
                                   : -1;
    const double missing = shortfall();
    const double picked = chosen();
    const std::vector<const SpaceItem *> picked_items = chosen_items();

    // A row of buttons from (x, y), the first primary; wraps when it runs out of room.
    const auto button_row = [&](float y, ui::ButtonKind first, const std::vector<bool> &enabled)
    {
        float x = kLeft;
        for (std::size_t b = 0; b < labels.size(); ++b)
        {
            const ui::ButtonKind kind = b == 0 ? first : ui::ButtonKind::secondary;
            const float w = ui::button_width(type, labels[b], 21.0f, kind);
            if (x > kLeft && x + w > kLeft + kWidth)
            {
                x = kLeft;
                y += ui::kButtonHeight + 16.0f;
            }
            ui::button(overlay, type, {x, y, w, ui::kButtonHeight}, labels[b], kind,
                       static_cast<int>(b) == focused_button ? 1.0f : 0.0f, b < enabled.size() ? enabled[b] : true, 21.0f);
            x += w + 16.0f;
        }
        return y + ui::kButtonHeight;
    };
    const auto meter = [&](float y, const std::string &label, double fraction, bool met)
    {
        ui::text_fit(overlay, type.medium, label, kLeft, y, 21.0f, kWidth, met ? ui::color::text : ui::color::text2);
        const ui::Rect bar{kLeft, y + 18.0f, kWidth, 8.0f};
        overlay.rounded_rect(bar, 4.0f, ui::color::field);
        ui::progress_bar(overlay, bar, static_cast<float>(std::clamp(fraction, 0.0, 1.0)), ui::color::accent);
    };

    if (step_ == Step::shortfall)
    {
        float y = kTop + 112.0f;
        y = ui::paragraph(overlay, type.regular,
                          tr("Needs {size} \xC2\xB7 {free} free on {drive} \xC2\xB7 {missing} more needed",
                             {{"size", format_bytes(request_.size_bytes)},
                              {"free", format_bytes(std::max(0.0, projected_free()))},
                              {"drive", drive_name},
                              {"missing", format_bytes(missing)}}),
                          kLeft, y, 24.0f, kWidth, 36.0f, ui::color::text, 3);
        if (drive != nullptr && drive->pending_bytes > 0.0)
            y = ui::paragraph(overlay, type.regular, tr("Unfinished downloads keep the room they still need."), kLeft,
                              y + 6.0f, 20.0f, kWidth, 30.0f, ui::color::text3, 2);
        button_row(y + 40.0f, ui::ButtonKind::primary, {});
    }
    else if (step_ == Step::list)
    {
        float y = kTop + 84.0f;
        int index = 0;
        if (!list_rows.empty() && list_rows.front().kind == 0)
        {
            // The drive, when Library opened the form and there's more than one.
            const ui::Rect r{kLeft, y, kWidth, kRow - 12.0f};
            const bool on = row_ == 0;
            ui::focus_ring(overlay, r, 14.0f, on ? 1.0f : 0.0f);
            ui::surface(overlay, r, 14.0f, on ? ui::color::field_open : ui::color::field);
            ui::text_fit(overlay, type.regular, tr("Drive"), r.x + 24.0f, r.cy() + 8.0f, 20.0f, 200.0f, ui::color::text2);
            const std::string value =
                drive == nullptr ? drive_name
                                 : drive_name + " \xC2\xB7 " + tr("{size} free", {{"size", format_bytes(drive->free_bytes)}});
            ui::text_fit(overlay, type.medium, value, r.x + 240.0f, r.cy() + 8.0f, 22.0f, r.w - 320.0f, ui::color::text);
            {
                // The fonts have no ‹ ›, so the icon set's arrows say Left and Right switch it.
                icons::draw(overlay, icons::Icon::back, r.x + r.w - 62.0f, r.cy(), 20.0f, ui::color::text2, 2.0f);
                icons::draw(overlay, icons::Icon::next, r.x + r.w - 34.0f, r.cy(), 20.0f, ui::color::text2, 2.0f);
            }
            y += kRow;
            index = 1;
        }
        if (request_.need_bytes > 0.0)
            meter(y + 20.0f,
                  tr("{selected} selected of {missing} needed",
                     {{"selected", format_bytes(picked)}, {"missing", format_bytes(missing)}}),
                  missing > 0.0 ? picked / missing : 1.0, picked >= missing);
        else if (list != nullptr)
            ui::text_fit(overlay, type.medium,
                         tr("{free} free of {total}",
                            {{"free", format_bytes(list->free_bytes)}, {"total", format_bytes(list->total_bytes)}}) +
                             " \xC2\xB7 " + tr("{size} selected", {{"size", format_bytes(picked)}}),
                         kLeft, y + 20.0f, 21.0f, kWidth, ui::color::text2);
        const float top = kTop + 200.0f + (index == 1 ? kRow : 0.0f);
        if (list == nullptr && error_.empty())
            ui::text(overlay, type.regular, tr("Checking this drive\xE2\x80\xA6"), kLeft, top + 30.0f, 21.0f,
                     ui::color::text3);
        else if (list != nullptr && list->leftovers.empty() && list->games.empty())
            ui::text(overlay, type.regular, tr("Orbit has nothing it can delete on this drive."), kLeft, top + 30.0f,
                     21.0f, ui::color::text3);
        if (list != nullptr)
        {
            overlay.push_clip({0.0f, top - 8.0f, 1920.0f, kListEnd - top + 8.0f});
            const float base = top - scroll_.value;
            const auto heading = [&](const char *label, float at)
            {
                ui::text(overlay, type.semibold, label, kLeft, base + at + 26.0f, 19.0f, ui::color::text3);
            };
            if (!list->leftovers.empty())
                heading(tr("Left by Orbit"), 0.0f);
            if (!list->games.empty())
                heading(tr("Games"), row_top(static_cast<int>(list->leftovers.size())) - kHeading);
            const int count = static_cast<int>(list->leftovers.size() + list->games.size());
            for (int i = 0; i < count; ++i)
            {
                const SpaceItem &item = i < static_cast<int>(list->leftovers.size())
                                            ? list->leftovers[static_cast<std::size_t>(i)]
                                            : list->games[static_cast<std::size_t>(i) - list->leftovers.size()];
                const ui::Rect r{kLeft, base + row_top(i), kWidth, kRow - 10.0f};
                if (r.y + r.h < top - 8.0f || r.y > kListEnd)
                    continue;
                const bool on = row_ == i + index;
                const bool ticked = has(selected_, item.id);
                ui::focus_ring(overlay, r, 14.0f, on ? 1.0f : 0.0f);
                ui::surface(overlay, r, 14.0f, on ? ui::color::field_open : ui::color::field);
                const ui::Rect box{r.x + 20.0f, r.cy() - 15.0f, 30.0f, 30.0f};
                // A round tick, as the browser's .space-check.
                if (ticked)
                {
                    overlay.rounded_rect(box, 15.0f, ui::color::action);
                    ui::icon_check(overlay, box.cx(), box.cy(), 18.0f, ui::color::night0, 3.0f);
                }
                else
                    overlay.bordered_rect(box, 15.0f, hui::gfx::Color{0, 0, 0, 0}, 2.0f, ui::color::text3);
                const std::string size = format_bytes(item.size_bytes);
                const float size_w = ui::text(overlay, type.medium, size, r.x + r.w - 24.0f, r.cy() + 8.0f, 21.0f,
                                              ui::color::text2, ui::Align::right);
                const float text_w = r.w - 70.0f - size_w - 48.0f;
                ui::text_fit(overlay, type.medium, item.title.empty() ? item.name : item.title, r.x + 70.0f,
                             r.cy() - 2.0f, 21.0f, text_w, ui::color::text);
                ui::text_fit(overlay, type.regular, item_sub(item), r.x + 70.0f, r.cy() + 22.0f, 17.0f, text_w,
                             ui::color::text3);
            }
            overlay.pop_clip();
        }
        if (!error_.empty())
            ui::text_fit(overlay, type.regular, error_, kLeft, kListEnd + 36.0f, 20.0f, kWidth, ui::color::bad);
        else if (!notice_.empty())
            ui::text_fit(overlay, type.regular, notice_, kLeft, kListEnd + 36.0f, 20.0f, kWidth, ui::color::text);
        const bool ready = waiting_ == 0 && !picked_items.empty() && picked >= missing;
        button_row(kButtons, ui::ButtonKind::primary, {ready, true});
    }
    else if (step_ == Step::confirm)
    {
        float y = ui::paragraph(
            overlay, type.regular,
            trn(static_cast<long>(picked_items.size()), "Delete {count} item, {size}?", "Delete {count} items, {size}?",
                {{"size", format_bytes(picked)}}) +
                " " + tr("Saves are not touched."),
            kLeft, kTop + 112.0f, 24.0f, kWidth, 36.0f, ui::color::text, 3);
        y += 14.0f;
        const std::size_t room = static_cast<std::size_t>((kListEnd - y) / 46.0f);
        for (std::size_t i = 0; i < picked_items.size() && i < room; ++i)
        {
            const SpaceItem &item = *picked_items[i];
            const bool more = i + 1 == room && picked_items.size() > room;
            const std::string title = more ? std::string("\xE2\x80\xA6") : item.title.empty() ? item.name : item.title;
            if (!more)
                ui::text(overlay, type.medium, format_bytes(item.size_bytes), kLeft + kWidth, y + 30.0f, 21.0f,
                         ui::color::text2, ui::Align::right);
            ui::text_fit(overlay, type.medium, title, kLeft, y + 30.0f, 21.0f, kWidth - 200.0f, ui::color::text);
            overlay.rounded_rect({kLeft, y + 45.0f, kWidth, 1.0f}, 0.0f, ui::color::line);
            y += 46.0f;
        }
        if (!error_.empty())
            ui::text_fit(overlay, type.regular, error_, kLeft, kListEnd + 36.0f, 20.0f, kWidth, ui::color::bad);
        button_row(kButtons, ui::ButtonKind::danger, {waiting_ == 0, waiting_ != 3});
    }
    else
    {
        const SpaceDeletion &d = state.space.deletion;
        ui::text_fit(overlay, type.regular,
                     d.current.empty() ? std::string(tr("Deleting\xE2\x80\xA6"))
                                       : tr("Deleting {title}\xE2\x80\xA6", {{"title", d.current}}),
                     kLeft, kTop + 120.0f, 24.0f, kWidth, ui::color::text);
        float y = kTop + 170.0f;
        if (d.total > 0)
        {
            meter(y, tr("{done} of {total}", {{"done", i18n::count(d.done)}, {"total", i18n::count(d.total)}}),
                  static_cast<double>(d.done) / d.total, false);
            y += 50.0f;
        }
        button_row(y + 30.0f, ui::ButtonKind::secondary, {});
    }
    overlay.pop_opacity();
}

std::vector<hui::ui::Hint> FreeSpaceForm::hints() const
{
    switch (step_)
    {
    case Step::shortfall:
        return {{hui::ui::Button::cross, tr("Choose")}, {hui::ui::Button::circle, tr("Cancel")}};
    case Step::list:
        return {{hui::ui::Button::cross, tr("Select")}, {hui::ui::Button::circle, tr("Cancel")}};
    case Step::confirm:
        return {{hui::ui::Button::cross, tr("Choose")}, {hui::ui::Button::circle, tr("Back")}};
    case Step::deleting:
        break;
    }
    return {{hui::ui::Button::circle, tr("Close")}};
}

} // namespace orbit
