// Orbit Store TV app - Get everything: a game with its DLC in one go.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The option Download would use and every DLC for that title ID on its platform, ticked;
// Download all adds them to Downloads as one bundle that installs in order (game, then
// DLC), then each row shows what happened to it. Store
// shows it in its Download options panel, Crossbar as a card beside the list, Cinema as a
// sheet and Panorama as a group of tiles. Closing it leaves the bundle adding the rest.

#include "orbit/screens.hpp"
#include "orbit/i18n.hpp"
#include "orbit/look.hpp"
#include "orbit/icons.hpp"
#include "ui/fonts.hpp"

#include <algorithm>

namespace orbit
{

namespace
{
constexpr int kEvCinemaColumns = 4; // Cinema's cards, four to a line
const ui::Color kAdded = ui::Color::rgb(0x8be0b0);

void refused(Feedback &feedback)
{
    feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.45f);
}

// What to do on a provider's page while it is open on the console, as for Download.
std::string page_steps()
{
    return std::string(tr("Complete verification if shown, then wait. Orbit will check the generated download link automatically.")) +
           " " + tr("Orbit then comes back and the next item starts.");
}

// The next game page starts on the source and format downloaded last, as Download does.
void remember(const Release &release)
{
    look::Prefs prefs = look::prefs();
    if (prefs.download_source == release.source_id && prefs.download_format == release_format(release))
        return;
    prefs.download_source = release.source_id;
    prefs.download_format = release_format(release);
    look::set(prefs);
}
} // namespace

// ---------------------------------------------------------------- the checklist

GamePage::EvPlan GamePage::ev_plan() const
{
    EvPlan plan;
    const Snapshot &state = ctx_.store.state();
    const Game *game = state.game(game_id_);
    if (game == nullptr || game->releases.empty())
        return plan;
    const Release &option = game->releases[static_cast<std::size_t>(std::clamp(option_, 0, static_cast<int>(game->releases.size()) - 1))];
    const std::vector<EverythingRow> rows = everything_rows(*game, option, state.addons, state.library_games(),
                                                            look::prefs().download_source, look::prefs().download_format);
    plan.summary = everything_summary(rows);
    std::vector<const Release *> chosen;
    for (const EverythingRow &r : rows)
    {
        EvRow row;
        row.release = r.release->id;
        row.option = r.release;
        row.kind = r.kind;
        row.label = everything_label(r);
        row.name = r.item != nullptr ? r.item->title : std::string();
        row.source = r.release->provider;
        row.size = format_bytes(r.release->size_bytes);
        row.installed = r.installed;
        row.enabled = !r.installed;
        row.ticked = row.enabled && !contains(ev_unticked_, row.release);
        row.detail = row.source;
        if (r.kind == EverythingRow::Kind::game && !r.release->version.empty())
            row.detail += " \xC2\xB7 v" + r.release->version;
        if (row.ticked)
        {
            ++plan.ticked;
            plan.total += r.release->size_bytes;
            chosen.push_back(r.release);
        }
        plan.rows.push_back(std::move(row));
    }
    // The drive Download would use: the one picked on the page, else the default for all of it
    // at once (one item: its own default). Only the downloads count, as for Download: room to
    // unpack an archive is checked just before it is unpacked.
    if (!drive_id_.empty() || chosen.size() <= 1)
        plan.drive = drive(chosen.empty() ? option : *chosen.front());
    else
    {
        Release all = *chosen.front();
        all.id.clear();
        all.size_bytes = plan.total;
        const int at = default_drive(state.drives, state.system.preferred_storage, all, state.jobs);
        plan.drive = at >= 0 ? &state.drives[static_cast<std::size_t>(at)] : nullptr;
    }
    if (plan.drive != nullptr)
        for (const Release *release : chosen)
            if (!plan_space(*release, *plan.drive, state.jobs).planned)
                plan.needed += release->size_bytes;
    plan.free = plan.drive != nullptr ? plan.drive->projected_free_bytes : 0.0;
    plan.enough = plan.drive != nullptr && plan.free - plan.needed >= kSpaceMargin;
    plan.can_start = plan.ticked > 0 && plan.enough && state.paired && !ctx_.store.bundle_action().busy;
    plan.error = ev_error_;
    if (!ev_sent_)
        return plan;
    // After Download all: the bundle's items, in its order, each in its state.
    plan.progress = true;
    plan.can_start = false;
    const Bundle *bundle = state.have_bundle && !ev_bundle_.empty() && state.bundle.id == ev_bundle_ ? &state.bundle : nullptr;
    std::vector<EvRow> shown;
    if (bundle != nullptr)
    {
        plan.running = bundle->running();
        plan.added = bundle->added();
        for (const BundleItem &item : bundle->items)
        {
            EvRow row;
            const auto known = std::find_if(plan.rows.begin(), plan.rows.end(),
                                            [&](const EvRow &r) { return r.release == item.release_id; });
            if (known != plan.rows.end())
                row = *known;
            else
            {
                row.release = item.release_id;
                row.label = item.title;
                row.kind = item.kind == "dlc" ? EverythingRow::Kind::dlc : EverythingRow::Kind::game;
            }
            row.state = item.state;
            if (item.state == "page")
                plan.page_provider = row.source;
            shown.push_back(std::move(row));
        }
        if (!bundle->message.empty() && plan.error.empty())
            plan.error = tr(std::string_view(bundle->message));
        // Rows say "Not added"; the note says why, for the first one (as the browser and Zero).
        for (const BundleItem &item : bundle->items)
            if (plan.error.empty() && (item.state == "failed" || item.state == "skipped") && !item.message.empty())
                plan.error = item.title + " \xC2\xB7 " + std::string(tr(std::string_view(item.message)));
    }
    else
    {
        // Sent, with no answer yet: what was ticked, waiting.
        plan.running = true;
        for (EvRow row : plan.rows)
            if (row.ticked)
            {
                row.state = "pending";
                shown.push_back(std::move(row));
            }
    }
    plan.rows = std::move(shown);
    return plan;
}

bool GamePage::ev_offered() const
{
    const Snapshot &state = ctx_.store.state();
    // Orbit Zero downloads on the computer, and a bundle is the console's own. An older
    // service has no bundles.
    if (state.system.zero_enabled || !state.paired || state.system.bundle_protocol < 1)
        return false;
    const Game *game = state.game(game_id_);
    if (game == nullptr || game->releases.empty() || !game->addon_id.empty())
        return false;
    const Release &option = game->releases[static_cast<std::size_t>(std::clamp(option_, 0, static_cast<int>(game->releases.size()) - 1))];
    return everything_offered(everything_rows(*game, option, state.addons, state.library_games(),
                                              look::prefs().download_source, look::prefs().download_format));
}

void GamePage::ev_open()
{
    ev_open_ = true;
    ev_error_.clear();
    ev_sent_ = false;
    ev_bundle_.clear();
    ev_buttons_ = false;
    ev_focus_ = 0;
    ev_serial_ = ctx_.store.bundle_action().serial;
    // A Download all still adding this game's items: show it rather than a new checklist.
    const Snapshot &state = ctx_.store.state();
    const EvPlan plan = ev_plan();
    if (state.have_bundle && state.bundle.running())
        for (const EvRow &row : plan.rows)
            if (state.bundle.item(row.release) != nullptr)
            {
                ev_sent_ = true;
                ev_bundle_ = state.bundle.id;
                break;
            }
    if (ev_sent_)
    {
        ev_buttons_ = true;
        ev_focus_ = 1; // Close
    }
    else
        for (int i = 0; i < static_cast<int>(plan.rows.size()); ++i)
            if (plan.rows[static_cast<std::size_t>(i)].enabled)
            {
                ev_focus_ = i;
                break;
            }
    ev_menu_.show(ev_focus_);
    ctx_.store.want_bundle(true);
    ctx_.store.refresh_now();
}

void GamePage::show_everything()
{
    ev_open();
    if (look::is(look::Interface::store))
        show_sheet(Sheet::everything, ev_sent_ ? Slot::ev_close : Slot::tick, ev_sent_ ? 0 : ev_focus_);
}

void GamePage::ev_close()
{
    ev_open_ = false;
    ev_menu_.open = false;
    ctx_.store.want_bundle(false);
}

void GamePage::ev_toggle(const EvPlan &plan, int row, Feedback &feedback)
{
    if (plan.progress || row < 0 || row >= static_cast<int>(plan.rows.size()) || !plan.rows[static_cast<std::size_t>(row)].enabled)
    {
        refused(feedback);
        return;
    }
    const std::string &id = plan.rows[static_cast<std::size_t>(row)].release;
    const auto at = std::find(ev_unticked_.begin(), ev_unticked_.end(), id);
    if (at == ev_unticked_.end())
        ev_unticked_.push_back(id);
    else
        ev_unticked_.erase(at);
    ev_error_.clear();
    feedback.play(hui::audio::Cue::toggle);
}

void GamePage::ev_start(const EvPlan &plan, Feedback &feedback)
{
    const Snapshot &state = ctx_.store.state();
    if (!plan.can_start || plan.drive == nullptr)
    {
        refused(feedback);
        return;
    }
    // In install order; each one as its own Download would send it.
    std::vector<api::Client::BundleItemRequest> items;
    for (const EvRow &row : plan.rows)
    {
        if (!row.ticked || row.option == nullptr)
            continue;
        const bool torbox = row.kind == EverythingRow::Kind::game ? torbox_ : state.debrid.always_for(*row.option);
        items.push_back({row.release, torbox ? "torbox" : "direct"});
        if (row.kind == EverythingRow::Kind::game)
            remember(*row.option);
    }
    ctx_.store.start_bundle(plan.drive->id, std::move(items), true);
    ev_sent_ = true;
    ev_bundle_.clear();
    ev_error_.clear();
    ev_buttons_ = true;
    ev_focus_ = 1;
    feedback.play(hui::audio::Cue::select);
}

void GamePage::ev_stop(Feedback &feedback)
{
    const Snapshot &state = ctx_.store.state();
    if (ev_bundle_.empty() || !state.have_bundle || state.bundle.id != ev_bundle_ || !state.bundle.running() ||
        ctx_.store.bundle_action().busy)
    {
        refused(feedback);
        return;
    }
    ctx_.store.stop_bundle(ev_bundle_);
    feedback.play(hui::audio::Cue::select);
}

void GamePage::ev_follow()
{
    const ActionState &action = ctx_.store.bundle_action();
    if (action.serial == ev_serial_ || action.busy)
        return;
    ev_serial_ = action.serial;
    if (!ev_open_ || !ev_sent_)
        return;
    if (!action.error.empty())
    {
        // Refused (another Download all, the space): back to the checklist with why.
        if (ev_bundle_.empty())
        {
            ev_sent_ = false;
            ev_buttons_ = false;
            ev_focus_ = 0;
        }
        ev_error_ = action.error;
        return;
    }
    if (ev_bundle_.empty())
        ev_bundle_ = action.created_job;
}

std::string GamePage::ev_total_line(const EvPlan &plan) const
{
    return tr("Total {total} \xC2\xB7 Free {free}", {{"total", format_bytes(plan.total)}, {"free", format_bytes(plan.free)}});
}

std::string GamePage::ev_note(const EvPlan &plan) const
{
    std::string note;
    if (plan.drive != nullptr)
        note = tr("To {drive}.", {{"drive", i18n::place(plan.drive->label)}}) + " ";
    return note + tr("Installs in order: game, then DLC.");
}

std::string GamePage::ev_heading(const EvPlan &plan) const
{
    if (plan.progress)
    {
        const std::string done = i18n::count(plan.added), total = i18n::count(static_cast<long>(plan.rows.size()));
        if (plan.running)
            return tr("Adding {done} of {total} to Downloads", {{"done", done}, {"total", total}});
        return tr("Added {done} of {total} to Downloads", {{"done", done}, {"total", total}});
    }
    const Game *game = ctx_.store.state().game(game_id_);
    if (game == nullptr || plan.rows.empty() || plan.rows.front().option == nullptr)
        return {};
    const Release &option = *plan.rows.front().option;
    return game->title + " \xC2\xB7 " + option.title_id + (option.region.empty() ? "" : " \xC2\xB7 " + option.region);
}

// ---------------------------------------------------------------- Store

// The rows, in the Download options panel's body (y from its top): the white-circle tick
// cards, each with its size; after Download all, each row's state instead of its tick.
void GamePage::compose_everything(hui::gfx::DrawList *list, Layout &out, bool focused) const
{
    const ui::Type &type = ctx_.type;
    const EvPlan plan = ev_plan();
    // Fixed at the bottom of the panel's right-hand box: Cancel and Download all, or Stop and Close.
    const auto fixed = [&](Slot slot, ui::Rect r, bool enabled) { out.controls.push_back({slot, 0, r, enabled, true}); };
    if (plan.progress)
    {
        if (plan.running)
            fixed(Slot::ev_stop, {1252.0f, 884.0f, 230.0f, 78.0f}, !ctx_.store.bundle_action().busy);
        fixed(Slot::ev_close, {plan.running ? 1500.0f : 1252.0f, 884.0f, plan.running ? 306.0f : 554.0f, 78.0f}, true);
    }
    else
    {
        fixed(Slot::ev_cancel, {1252.0f, 884.0f, 230.0f, 78.0f}, true);
        fixed(Slot::ev_go, {1500.0f, 884.0f, 306.0f, 78.0f}, plan.can_start);
    }
    float y = 0.0f;
    const std::string heading = ev_heading(plan);
    if (list != nullptr && !heading.empty())
        ui::text_fit(*list, type.regular, heading, 116.0f, y + 24.0f, 21.0f, 1040.0f, ui::color::text2);
    y += 52.0f;
    const float x = 116.0f, width = 1060.0f, height = 96.0f;
    for (int i = 0; i < static_cast<int>(plan.rows.size()); ++i)
    {
        const EvRow &row = plan.rows[static_cast<std::size_t>(i)];
        const ui::Rect r{x, y, width, height};
        const bool focusable = !plan.progress;
        const float focus = focusable && focused && static_cast<int>(out.controls.size()) == focus_ ? 1.0f : 0.0f;
        if (focusable)
            out.controls.push_back({Slot::tick, i, r, row.enabled, false});
        if (list != nullptr)
        {
            const bool page = plan.progress && row.state == "page";
            ui::focus_ring(*list, r, 20.0f, focus);
            ui::surface(*list, r, 20.0f, (row.ticked && !plan.progress) || page ? ui::color::field_open : ui::color::field);
            if (page)
                list->bordered_rect(r, 20.0f, ui::Color{0, 0, 0, 0}, 2.0f, ui::Color::rgb(0xffffff, 0.55f));
            list->push_opacity(row.enabled ? 1.0f : 0.45f);
            const float cx = x + 40.0f, cy = y + height * 0.5f;
            std::string right = row.installed ? std::string(tr("Installed")) : row.size;
            ui::Color right_ink = ui::color::text;
            std::string detail = row.detail;
            if (plan.progress)
            {
                // Added, the page open on the PS5, waiting, or not added.
                right = bundle_state_label(row.state);
                detail = row.source.empty() ? row.size : row.source + " \xC2\xB7 " + row.size;
                if (row.state == "added")
                {
                    list->circle(cx, cy, 15.0f, kAdded);
                    ui::icon_check(*list, cx, cy, 18.0f, ui::color::night1);
                    right_ink = kAdded;
                }
                else if (row.state == "page")
                    list->arc(cx, cy, 15.0f, 3.0f, 0.6f, 4.6f, ui::color::text);
                else
                {
                    list->ring(cx, cy, 14.0f, 1.5f, ui::color::text3);
                    right_ink = row.state == "failed" || row.state == "skipped" ? ui::color::text3 : ui::color::text2;
                }
            }
            else if (row.ticked)
            {
                list->circle(cx, cy, 15.0f, ui::color::text);
                ui::icon_check(*list, cx, cy, 18.0f, ui::color::night1);
            }
            else
                list->ring(cx, cy, 14.0f, 1.0f, ui::color::text3);
            const float right_w = std::min(420.0f, type.medium.measure(right, 22.0f));
            ui::text_fit(*list, type.medium, right, x + width - 28.0f, cy + 8.0f, 22.0f, 420.0f, right_ink, ui::Align::right);
            const float text_w = width - 74.0f - right_w - 56.0f;
            ui::text_fit(*list, type.medium, row.label, x + 74.0f, y + 41.0f, 25.0f, text_w, ui::color::text);
            ui::text_fit(*list, type.regular, detail, x + 74.0f, y + 73.0f, 19.0f, text_w, ui::color::text2);
            list->pop_opacity();
        }
        y += height + 14.0f;
    }
    out.end = y + 20.0f;
}

// The panel's right-hand box: the total and free space, where it goes and the install order
// (or, after Download all, the provider page to finish), then the buttons.
void GamePage::draw_everything_box(hui::gfx::DrawList &list, const Layout &where, bool focused) const
{
    const ui::Type &type = ctx_.type;
    const EvPlan plan = ev_plan();
    const ui::Rect box{1226.0f, 430.0f, 596.0f, 560.0f};
    list.rounded_rect(box, 22.0f, ui::Color::rgb(0x111111, 0.98f));
    const float x = box.x + 26.0f, w = box.w - 52.0f;
    float y = box.y + 52.0f;
    // After Download all, while no provider page is open, the box keeps its plan.
    if (!plan.progress || plan.page_provider.empty())
    {
        ui::text_fit(list, type.medium, ev_total_line(plan), x, y, 26.0f, w, ui::color::text);
        y = ui::paragraph(list, type.regular, ev_note(plan), x, y + 42.0f, 19.0f, w, 28.0f, ui::color::text2, 3) + 14.0f;
        if (!plan.progress && plan.drive == nullptr)
            y = ui::paragraph(list, type.regular, tr("Connect a writable drive to your PS5."), x, y + 14.0f, 19.0f, w, 28.0f,
                              ui::color::text, 2) + 14.0f;
        else if (!plan.progress && !plan.enough)
            y = ui::paragraph(list, type.medium, tr("Not enough space on this drive."), x, y + 14.0f, 19.0f, w, 28.0f,
                              ui::color::text, 2) + 14.0f;
    }
    else
    {
        // The provider's page, open on the console: finish it there.
        const std::string head = tr("The {provider} page is open on your PS5", {{"provider", plan.page_provider}});
        const std::string body = page_steps();
        const int n = std::min(4, static_cast<int>(type.regular.font->wrap(body, 18.0f, w - 76.0f).size()));
        const ui::Rect card{x, y - 30.0f, w, 72.0f + 26.0f * static_cast<float>(n)};
        list.rounded_rect(card, 16.0f, ui::Color::rgb(0xffffff, 0.06f));
        list.bordered_rect(card, 16.0f, ui::Color{0, 0, 0, 0}, 1.0f, ui::color::line);
        icons::draw(list, icons::Icon::tv, card.x + 34.0f, card.y + 36.0f, 26.0f, ui::color::text, 1.8f);
        ui::text_fit(list, type.medium, head, card.x + 62.0f, card.y + 44.0f, 20.0f, card.w - 80.0f, ui::color::text);
        ui::paragraph(list, type.regular, body, card.x + 62.0f, card.y + 74.0f, 18.0f, card.w - 80.0f, 26.0f,
                      ui::color::text2, n);
        y = card.y + card.h + 34.0f;
    }
    if (!plan.error.empty())
        ui::paragraph(list, type.regular, plan.error, x, y + 18.0f, 18.0f, w, 26.0f, ui::color::text, 3);
    for (std::size_t i = 0; i < where.controls.size(); ++i)
    {
        const Control &c = where.controls[i];
        if (!c.fixed || (c.slot != Slot::ev_go && c.slot != Slot::ev_cancel && c.slot != Slot::ev_stop && c.slot != Slot::ev_close))
            continue;
        const float focus = focused && static_cast<int>(i) == focus_ ? 1.0f : 0.0f;
        const bool light = c.slot == Slot::ev_go || c.slot == Slot::ev_close;
        const char *label = c.slot == Slot::ev_go       ? tr("Download all")
                            : c.slot == Slot::ev_cancel ? tr("Cancel")
                            : c.slot == Slot::ev_stop   ? tr("Stop")
                                                        : tr("Close");
        ui::button(list, type, c.rect, label, light ? ui::ButtonKind::light : ui::ButtonKind::secondary, focus, c.enabled,
                   ui::fit_size(type.medium, label, 23.0f, 17.0f, c.rect.w - 40.0f));
    }
}

// ---------------------------------------------------------------- the buttons

// Under the checklist: Cancel and Download all; once sent, Stop (while it adds) and Close.
std::vector<GamePage::EvButton> GamePage::ev_buttons(const EvPlan &plan) const
{
    if (!plan.progress)
        return {EvButton::cancel, EvButton::go};
    if (plan.running)
        return {EvButton::stop, EvButton::close};
    return {EvButton::close};
}

const char *GamePage::ev_button_label(EvButton button)
{
    switch (button)
    {
    case EvButton::cancel:
        return tr("Cancel");
    case EvButton::go:
        return tr("Download all");
    case EvButton::stop:
        return tr("Stop");
    default:
        return tr("Close");
    }
}

bool GamePage::ev_button_enabled(const EvPlan &plan, EvButton button) const
{
    if (button == EvButton::go)
        return plan.can_start;
    if (button == EvButton::stop)
        return plan.running && !ctx_.store.bundle_action().busy;
    return true;
}

void GamePage::ev_press(const EvPlan &plan, EvButton button, Feedback &feedback)
{
    if (!ev_button_enabled(plan, button))
    {
        refused(feedback);
        return;
    }
    if (button == EvButton::go)
        ev_start(plan, feedback);
    else if (button == EvButton::stop)
        ev_stop(feedback);
    else
    {
        ev_close();
        feedback.play(hui::audio::Cue::back);
    }
}

// Crossbar and Cinema: the focus on a row (or card), or on a button. Rows go up and down
// (Cinema: across, four to a line); the buttons sit side by side under them.
void GamePage::ev_navigate(const EvPlan &plan, const InputFrame &input, Feedback &feedback, int columns)
{
    const std::vector<EvButton> buttons = ev_buttons(plan);
    const int rows = plan.progress ? 0 : static_cast<int>(plan.rows.size());
    const int count = static_cast<int>(buttons.size());
    if (rows == 0)
        ev_buttons_ = true;
    ev_focus_ = std::clamp(ev_focus_, 0, std::max(0, (ev_buttons_ ? count : rows) - 1));
    const auto enabled_row = [&](int i) { return i >= 0 && i < rows && plan.rows[static_cast<std::size_t>(i)].enabled; };
    const auto move = [&](int to)
    {
        ev_focus_ = to;
        feedback.play(hui::audio::Cue::focus);
    };
    const bool across = input.nav == Direction::left || input.nav == Direction::right;
    const bool forward = input.nav == Direction::right || input.nav == Direction::down;
    if (input.nav == Direction::none)
        return;
    if (ev_buttons_)
    {
        if (across)
        {
            const int to = ev_focus_ + (forward ? 1 : -1);
            if (to >= 0 && to < count)
                move(to);
            else
                refuse(feedback, input);
        }
        else if (!forward && rows > 0)
        {
            // Up: back to the last row (the last line's first card in Cinema).
            int to = columns > 1 ? (rows - 1) / columns * columns : rows - 1;
            while (to >= 0 && !enabled_row(to))
                --to;
            if (to < 0)
                refuse(feedback, input);
            else
            {
                ev_buttons_ = false;
                move(to);
            }
        }
        else
            refuse(feedback, input);
        return;
    }
    const int step = across ? (forward ? 1 : -1) : (forward ? columns : -columns);
    int to = ev_focus_ + step;
    if (across && to / std::max(1, columns) != ev_focus_ / std::max(1, columns))
        to = -1; // the end of a line
    while (to >= 0 && to < rows && !enabled_row(to))
        to += step;
    if (to >= 0 && to < rows && (!across || to / std::max(1, columns) == ev_focus_ / std::max(1, columns)))
        move(to);
    else if (forward && !across)
    {
        // Down from the last line: the buttons, on Download all (or Close).
        ev_buttons_ = true;
        move(count - 1);
    }
    else
        refuse(feedback, input);
}

// ---------------------------------------------------------------- Crossbar

// The Get everything row's value: what it adds and how much.
std::string GamePage::ev_row_value() const
{
    const EvPlan plan = ev_plan();
    return plan.summary.empty() ? std::string() : plan.summary + " \xC2\xB7 " + format_bytes(plan.total);
}

Intent GamePage::ev_update_crossbar(const InputFrame &input, Feedback &feedback)
{
    Intent intent;
    const EvPlan plan = ev_plan();
    if (input.is_pressed(Action::back))
    {
        ev_close();
        feedback.play(hui::audio::Cue::back);
        return intent;
    }
    ev_navigate(plan, input, feedback, 1);
    if (input.is_pressed(Action::confirm))
    {
        if (!ev_buttons_)
            ev_toggle(plan, ev_focus_, feedback);
        else
        {
            const std::vector<EvButton> buttons = ev_buttons(plan);
            ev_press(plan, buttons[static_cast<std::size_t>(std::clamp(ev_focus_, 0, static_cast<int>(buttons.size()) - 1))],
                     feedback);
        }
    }
    return intent;
}

// The design's glass card on the right: a small title and the game's line, rows with square
// ticks (the focused one outlined), the totals, then Cancel and Download all with its size.
void GamePage::ev_draw_crossbar(hui::gfx::DrawList &list) const
{
    const float in = ev_menu_.in.value;
    if (in <= 0.01f)
        return;
    const ui::Type &type = ctx_.type;
    const EvPlan plan = ev_plan();
    const auto &regular = [&](std::string_view text) -> const hui::ui::FontRef & { return ui::face(type.sora, type.regular, text); };
    const auto &bold = [&](std::string_view text) -> const hui::ui::FontRef & { return ui::face(type.sora_semibold, type.semibold, text); };
    const float w = 645.0f, x = 1920.0f - 96.0f - w, pad = 33.0f, inner = w - 2.0f * pad;
    const float row_h = 69.0f, row_gap = 15.0f;
    // The note under the totals: where it goes and the order, or what's happening.
    std::string head, note;
    if (plan.progress)
    {
        head = ev_heading(plan);
        note = !plan.page_provider.empty()
                   ? tr("The {provider} page is open on your PS5", {{"provider", plan.page_provider}}) + ". " +
                         page_steps()
                   : std::string();
    }
    else
    {
        head = ev_total_line(plan);
        note = plan.drive == nullptr ? std::string(tr("Connect a writable drive to your PS5."))
               : !plan.enough       ? std::string(tr("Not enough space on this drive.")) + " " + ev_note(plan)
                                    : ev_note(plan);
    }
    if (!plan.error.empty())
        note = note.empty() ? plan.error : plan.error + " " + note;
    const int note_lines = std::min(4, static_cast<int>(regular(note).font->wrap(note, 18.0f, inner - 12.0f).size()));
    const float rows_h = static_cast<float>(plan.rows.size()) * (row_h + row_gap);
    const float h = pad + 60.0f + rows_h + 15.0f + 18.0f + 34.0f + 27.0f * static_cast<float>(note_lines) + 24.0f + 60.0f + pad;
    const float y0 = std::max(120.0f, std::min(225.0f, 1040.0f - h));
    const ui::Rect card{x, y0, w, h};
    list.push_opacity(in);
    list.shadow({card.x, card.y + 30.0f, card.w, card.h}, 33.0f, 75.0f, ui::Color::rgb(0x000000, 0.3f));
    list.rounded_rect(card, 33.0f, ui::Color::rgb(0x0a0c2c, 0.62f));
    float y = y0 + pad;
    hui::ui::text(list, type.semibold, hui::ui::upper(tr("Get everything")), x + pad + 6.0f, y + 14.0f, 16.5f,
                  ui::Color::rgb(0xffffff, 0.7f), ui::Align::left, 2.6f);
    const Game *game = ctx_.store.state().game(game_id_);
    std::string line;
    if (game != nullptr && !plan.rows.empty() && plan.rows.front().option != nullptr)
        line = game->title + " \xC2\xB7 " + plan.rows.front().option->title_id;
    ui::text_fit(list, regular(line), line, x + pad + 6.0f, y + 44.0f, 22.5f, inner - 12.0f, ui::Color::rgb(0xffffff));
    y += 60.0f;
    for (int i = 0; i < static_cast<int>(plan.rows.size()); ++i)
    {
        const EvRow &row = plan.rows[static_cast<std::size_t>(i)];
        const ui::Rect r{x + pad, y, inner, row_h};
        const bool on = !plan.progress && !ev_buttons_ && i == ev_focus_;
        const bool page = plan.progress && row.state == "page";
        if (on || page)
        {
            list.rounded_rect(r, 18.0f, ui::Color::rgb(0xffffff, 0.2f));
            list.bordered_rect(r.inset(-1.5f), 19.5f, ui::Color{0, 0, 0, 0}, 3.0f, ui::Color::rgb(0xffffff, on ? 1.0f : 0.55f));
        }
        list.push_opacity(row.enabled || plan.progress ? 1.0f : 0.42f);
        const float mid = r.cy();
        const ui::Rect box{r.x + 18.0f, mid - 15.0f, 30.0f, 30.0f};
        std::string right = row.installed ? std::string(tr("Installed")) : row.size;
        ui::Color right_ink = ui::Color::rgb(0xffffff);
        if (plan.progress)
        {
            right = bundle_state_label(row.state);
            if (row.state == "added")
            {
                list.rounded_rect(box, 9.0f, kAdded);
                ui::icon_check(list, box.cx(), mid, 19.0f, ui::Color::rgb(0x141433), 3.4f);
                right_ink = kAdded;
            }
            else if (row.state == "page")
                list.arc(box.cx(), mid, 15.0f, 3.0f, 0.6f, 4.6f, ui::Color::rgb(0xffffff));
            else
            {
                list.bordered_rect(box, 9.0f, ui::Color{0, 0, 0, 0}, 2.0f, ui::Color::rgb(0xffffff, 0.45f));
                right_ink = ui::Color::rgb(0xffffff, 0.6f);
            }
        }
        else if (row.ticked)
        {
            list.rounded_rect(box, 9.0f, ui::Color::rgb(0xffffff));
            ui::icon_check(list, box.cx(), mid, 19.0f, ui::Color::rgb(0x141433), 3.4f);
        }
        else
            list.bordered_rect(box, 9.0f, ui::Color{0, 0, 0, 0}, 2.0f, ui::Color::rgb(0xffffff, 0.6f));
        const float right_w = std::min(300.0f, regular(right).measure(right, 21.0f));
        ui::text_fit(list, regular(right), right, r.x + r.w - 18.0f, mid + 7.0f, 21.0f, 300.0f, right_ink, ui::Align::right);
        // "Game · Vikingfile": the label, then the source smaller and dimmer.
        const float tx = r.x + 18.0f + 30.0f + 18.0f;
        const float room = r.x + r.w - 18.0f - right_w - 24.0f - tx;
        const float label_w = ui::text_fit(list, bold(row.label), row.label, tx, mid + 8.0f, 22.5f, room, ui::Color::rgb(0xffffff));
        if (!row.source.empty() && label_w + 40.0f < room)
            ui::text_fit(list, regular(row.source), " \xC2\xB7 " + row.source, tx + label_w, mid + 8.0f, 19.5f, room - label_w,
                         ui::Color::rgb(0xffffff, 0.7f));
        list.pop_opacity();
        y += row_h + row_gap;
    }
    y += 0.0f;
    list.rounded_rect({x + pad, y, inner, 1.5f}, 0.0f, ui::Color::rgb(0xffffff, 0.16f));
    y += 18.0f;
    ui::text_fit(list, bold(head), head, x + pad + 6.0f, y + 24.0f, 22.5f, inner - 12.0f, ui::Color::rgb(0xffffff));
    y += 34.0f;
    ui::paragraph(list, regular(note), note, x + pad + 6.0f, y + 18.0f, 18.0f, inner - 12.0f, 27.0f,
                  ui::Color::rgb(0xffffff, 0.72f), note_lines);
    y += 27.0f * static_cast<float>(note_lines) + 24.0f;
    // The buttons: pills, the last one white (Download all with the total, or Close).
    float bx = x + pad;
    const std::vector<EvButton> buttons = ev_buttons(plan);
    for (int i = 0; i < static_cast<int>(buttons.size()); ++i)
    {
        const EvButton b = buttons[static_cast<std::size_t>(i)];
        std::string label = ev_button_label(b);
        if (b == EvButton::go)
            label += " \xC2\xB7 " + format_bytes(plan.total);
        const bool light = i + 1 == static_cast<int>(buttons.size());
        const float bw = bold(label).measure(label, 21.0f) + (light ? 72.0f : 60.0f);
        const ui::Rect r{bx, y, bw, 60.0f};
        const bool on = ev_buttons_ && i == ev_focus_;
        const bool enabled = ev_button_enabled(plan, b);
        if (on)
            list.bordered_rect(r.inset(-5.0f), 35.0f, ui::Color{0, 0, 0, 0}, 3.0f, ui::Color::rgb(0xffffff));
        list.push_opacity(enabled ? 1.0f : 0.42f);
        list.rounded_rect(r, 30.0f, light ? ui::Color::rgb(0xffffff) : ui::Color::rgb(0xffffff, 0.14f));
        ui::text(list, bold(label), label, r.cx(), r.cy() + 7.5f, 21.0f, light ? ui::Color::rgb(0x141433) : ui::Color::rgb(0xffffff),
                 ui::Align::center);
        list.pop_opacity();
        bx += bw + 15.0f;
    }
    list.pop_opacity();
}

// ---------------------------------------------------------------- Cinema

Intent GamePage::ev_update_cinema(const InputFrame &input, Feedback &feedback)
{
    Intent intent;
    const EvPlan plan = ev_plan();
    if (input.is_pressed(Action::back))
    {
        ev_close();
        feedback.play(hui::audio::Cue::back);
        return intent;
    }
    ev_navigate(plan, input, feedback, kEvCinemaColumns);
    if (input.is_pressed(Action::confirm))
    {
        if (!ev_buttons_)
            ev_toggle(plan, ev_focus_, feedback);
        else
        {
            const std::vector<EvButton> buttons = ev_buttons(plan);
            ev_press(plan, buttons[static_cast<std::size_t>(std::clamp(ev_focus_, 0, static_cast<int>(buttons.size()) - 1))],
                     feedback);
        }
    }
    return intent;
}

// The sheet (as the download's): on the left its title, the note box with the totals and the
// buttons at the bottom; on the right "Included" over a card per row, four to a line, each
// with its kind in gold capitals, its size and source and a gold tick.
void GamePage::ev_draw_cinema(hui::gfx::DrawList &list) const
{
    const ui::Type &type = ctx_.type;
    const EvPlan plan = ev_plan();
    const float top = cn::draw_sheet(list, 690.0f) + 56.0f;
    const float left = 100.0f, left_w = 560.0f;
    const float right = left + left_w + 70.0f, right_w = 1820.0f - right;
    const auto &title_font = ui::face(type.cinema, type.semibold, tr("Get everything"));
    ui::text(list, title_font, tr("Get everything"), left, top + 34.0f, 42.0f, cn::kInk);
    // The note box: the total and free space and where it goes, or what Download all is doing.
    std::string head, body;
    bool warn = false;
    if (plan.progress)
    {
        head = ev_heading(plan);
        body = !plan.page_provider.empty()
                   ? tr("The {provider} page is open on your PS5", {{"provider", plan.page_provider}}) + ". " +
                         page_steps()
                   : std::string();
    }
    else
    {
        head = ev_total_line(plan);
        body = ev_note(plan);
        if (plan.drive == nullptr)
            body = tr("Connect a writable drive to your PS5."), warn = true;
        else if (!plan.enough)
            body = std::string(tr("Not enough space on this drive.")) + " " + body, warn = true;
    }
    if (!plan.error.empty())
        body = body.empty() ? plan.error : plan.error + " " + body, warn = true;
    const float box_y = top + 70.0f;
    const int n = std::min(6, static_cast<int>(type.regular.font->wrap(body, 19.5f, left_w - 42.0f).size()));
    const ui::Rect box{left, box_y, left_w, 18.0f + 34.0f + 28.3f * static_cast<float>(n) + 18.0f};
    list.rounded_rect(box, 18.0f, ui::Color::rgb(0xf7f3ec, 0.06f));
    ui::text_fit(list, type.medium, head, box.x + 21.0f, box.y + 18.0f + 22.0f, 24.0f, box.w - 42.0f, cn::kInk);
    ui::paragraph(list, type.regular, body, box.x + 21.0f, box.y + 18.0f + 34.0f + 21.0f, 19.5f, box.w - 42.0f, 28.3f,
                  warn ? cn::kWarn : cn::kInk, n);
    // The buttons: outlined Cancel (or Stop), cream Download all (or Close).
    const std::vector<EvButton> buttons = ev_buttons(plan);
    float bx = left;
    const float by = 1080.0f - 60.0f - 66.0f;
    for (int i = 0; i < static_cast<int>(buttons.size()); ++i)
    {
        const EvButton b = buttons[static_cast<std::size_t>(i)];
        const std::string label = ev_button_label(b);
        const bool light = i + 1 == static_cast<int>(buttons.size());
        const float bw = type.medium.measure(label, 21.0f) + (light ? 72.0f : 60.0f);
        const ui::Rect r{bx, by, bw, 66.0f};
        const bool on = ev_buttons_ && i == ev_focus_;
        list.push_opacity(ev_button_enabled(plan, b) ? 1.0f : 0.42f);
        if (on)
        {
            list.glow(r, 33.0f, 27.0f, ui::Color::rgb(0xf7f3ec, 0.35f));
            list.bordered_rect(r.inset(-5.0f), 38.0f, ui::Color{0, 0, 0, 0}, 3.0f, cn::kInk);
        }
        if (light)
            list.rounded_rect(r, 33.0f, cn::kInk);
        else
            list.bordered_rect(r, 33.0f, ui::Color::rgb(0x000000, 0.32f), 2.25f, ui::Color::rgb(0xf7f3ec, 0.45f));
        ui::text(list, type.medium, label, r.cx(), r.cy() + 7.5f, 21.0f, light ? cn::kDark : cn::kInk, ui::Align::center);
        list.pop_opacity();
        bx += bw + 15.0f;
    }

    // Included: one card each.
    cn::kick(ctx_, list, right, top, tr("Included"), 16.5f);
    const float gap = 18.0f;
    const float cw = (right_w - gap * (kEvCinemaColumns - 1)) / static_cast<float>(kEvCinemaColumns);
    const float ch = 168.0f;
    for (int i = 0; i < static_cast<int>(plan.rows.size()); ++i)
    {
        const EvRow &row = plan.rows[static_cast<std::size_t>(i)];
        const ui::Rect r{right + static_cast<float>(i % kEvCinemaColumns) * (cw + gap),
                         top + 34.0f + static_cast<float>(i / kEvCinemaColumns) * (ch + gap), cw, ch};
        const bool focus = !plan.progress && !ev_buttons_ && i == ev_focus_;
        const bool page = plan.progress && row.state == "page";
        list.push_opacity(row.enabled || plan.progress ? 1.0f : 0.45f);
        list.rounded_rect(r, 21.0f, ui::Color::rgb(0xf7f3ec, focus || page ? 0.1f : 0.06f));
        if (focus || page)
            list.bordered_rect(r, 21.0f, ui::Color{0, 0, 0, 0}, 3.0f, ui::Color::rgb(0xf7f3ec, focus ? 1.0f : 0.55f));
        const float inner = r.w - 42.0f;
        // The tick: a gold disc; after Download all, green once added, a ring while waiting.
        const float tx = r.x + r.w - 18.0f - 16.5f, ty = r.y + 18.0f + 16.5f;
        if (plan.progress)
        {
            if (row.state == "added")
            {
                list.circle(tx, ty, 16.5f, kAdded);
                ui::icon_check(list, tx, ty, 19.0f, cn::kDark, 3.0f);
            }
            else if (row.state == "page")
                list.arc(tx, ty, 16.5f, 3.0f, 0.6f, 4.6f, cn::kInk);
            else
                list.ring(tx, ty, 15.5f, 2.0f, cn::kFaint);
        }
        else if (row.ticked)
        {
            list.circle(tx, ty, 16.5f, cn::kGold);
            ui::icon_check(list, tx, ty, 19.0f, cn::kDark, 3.0f);
        }
        else
            list.ring(tx, ty, 15.5f, 2.0f, cn::kFaint);
        // The kind in gold capitals; a DLC's name under it, clear of the tick.
        const bool dlc = row.kind == EverythingRow::Kind::dlc;
        cn::kick(ctx_, list, r.x + 21.0f, r.y + 21.0f, dlc ? std::string(tr("DLC")) : row.label, 16.5f);
        if (dlc)
            ui::text_fit(list, type.medium, row.name, r.x + 21.0f, r.y + 21.0f + 50.0f, 20.0f, inner, cn::kInk);
        const std::string title = plan.progress ? std::string(bundle_state_label(row.state))
                                  : row.installed ? std::string(tr("Installed"))
                                                  : row.size;
        const ui::Color title_ink = plan.progress && row.state == "added" ? kAdded : cn::kInk;
        ui::text_fit(list, type.medium, title, r.x + 21.0f, r.y + ch - 21.0f - 27.0f - 6.0f, 24.0f, inner, title_ink);
        const std::string sub = plan.progress ? row.source + " \xC2\xB7 " + row.size : row.source;
        ui::text_fit(list, type.regular, sub, r.x + 21.0f, r.y + ch - 21.0f - 4.0f, 18.0f, inner, ui::Color::rgb(0xf7f3ec, 0.7f));
        list.pop_opacity();
    }
}

// ---------------------------------------------------------------- Panorama

// Its own group beside the download: a wide tile per row (a corner check while ticked), two
// to a line, then Download all in the accent and the totals as text.
pano::Group GamePage::ev_group() const
{
    pano::Group group;
    const EvPlan plan = ev_plan();
    group.title = pano::lower(tr("Get everything"));
    group.count = i18n::count(static_cast<long>(plan.rows.size()));
    group.pattern = pano::Pattern::custom;
    const int rows = static_cast<int>(plan.rows.size());
    const int columns = std::max(2, (rows + 1) / 2);
    for (int i = 0; i < rows; ++i)
        group.places.push_back({(i % columns) * 2, i / columns, 2, 1});
    const int last = (rows + columns - 1) / columns;
    group.places.push_back({0, last, 2, 1});
    group.places.push_back({2, last, 2, 1});
    group.size = rows + 2;
    group.cols = columns * 2;
    const std::string total = plan.progress ? ev_heading(plan) : ev_total_line(plan);
    std::string note = plan.progress ? (!plan.page_provider.empty()
                                            ? tr("The {provider} page is open on your PS5", {{"provider", plan.page_provider}})
                                            : std::string())
                                     : plan.drive == nullptr ? std::string(tr("Connect a writable drive to your PS5."))
                                     : !plan.enough           ? std::string(tr("Not enough space on this drive."))
                                                              : std::string(tr("Installs in order: game, then DLC."));
    if (!plan.error.empty())
        note = plan.error;
    const std::string go_sub =
        plan.drive != nullptr
            ? tr("{size} \xC2\xB7 to {drive}", {{"size", format_bytes(plan.total)}, {"drive", i18n::place(plan.drive->label)}})
            : format_bytes(plan.total);
    const bool busy = ctx_.store.bundle_action().busy;
    group.focusable = [rows](int i) { return i <= rows; };
    group.fill = [plan, rows, total, note, go_sub, busy](int i, pano::Tile &tile)
    {
        if (i < rows)
        {
            const EvRow &row = plan.rows[static_cast<std::size_t>(i)];
            tile.key = "ev:" + std::to_string(i);
            tile.kicker = row.label;
            if (plan.progress)
            {
                tile.label = bundle_state_label(row.state);
                tile.sub = row.source + " \xC2\xB7 " + row.size;
                tile.check = row.state == "added";
                tile.selected = row.state == "page";
                tile.off = row.state == "failed" || row.state == "skipped";
            }
            else
            {
                tile.label = row.installed ? std::string(tr("Installed")) : row.size;
                tile.sub = row.detail;
                tile.check = row.ticked;
                tile.off = !row.enabled;
            }
            return;
        }
        if (i == rows)
        {
            // Download all, then Stop while it adds, and Close once it's done.
            tile.key = "ev:go";
            tile.has_icon = true;
            tile.has_bg = true;
            if (!plan.progress)
            {
                tile.icon = icons::Icon::down;
                tile.label = tr("Download all");
                tile.sub = go_sub;
                tile.bg = plan.can_start ? look::accent_color() : ui::Color::rgb(0x3a3c42);
            }
            else if (plan.running)
            {
                tile.icon = icons::Icon::stop;
                tile.label = tr("Stop");
                tile.bg = busy ? ui::Color::rgb(0x3a3c42) : look::accent_color();
            }
            else
            {
                tile.icon = icons::Icon::close;
                tile.label = tr("Close");
                tile.bg = look::accent_color();
            }
            return;
        }
        // The totals, as text.
        tile.key = "ev:total";
        tile.has_bg = true;
        tile.bg = ui::Color{0, 0, 0, 0};
        tile.label = total;
        tile.sub = note;
    };
    return group;
}

} // namespace orbit
