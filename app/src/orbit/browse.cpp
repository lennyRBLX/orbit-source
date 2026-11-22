// Orbit Store TV app - Browse: search, filters and every game as a grid.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The browser storefront's Browse (src/App.tsx, src/browse.ts): a search by
// name or title ID, All games or Favourites, then Source, Format, Region, Download
// size and Sort by, and Reset filters. Each filter is a dropdown here too.

#include "orbit/screens.hpp"
#include "orbit/i18n.hpp"
#include "orbit/json.hpp"
#include "orbit/look.hpp"

#include <algorithm>
#include <cstdio>

namespace orbit
{

namespace
{

// Shared 1920px geometry: six square covers and one compact toolbar.
constexpr int kColumns = 6;
constexpr float kTile = 268.333f;
constexpr float kColumnStep = (1920.0f - 2.0f * ui::kGutter - kTile) / (kColumns - 1);
constexpr float kRowStep = 374.0f;
constexpr float kTitleBaseline = 176.0f; // semibold page title
constexpr float kFieldY = 122.0f;
constexpr float kFieldWidth = 514.0f; // search beside the heading, after Saved filters and Reset filters
constexpr float kFieldHeight = 66.0f;
constexpr float kChipsY = 218.0f; // collection tabs share the filter row
constexpr float kChipHeight = 58.0f;
constexpr float kFiltersY = 216.0f; // .browse-selects
constexpr float kResetWidth = 152.0f;
constexpr float kFilterGap = 14.0f;
constexpr float kFilterWidth = (1920.0f - ui::kGutter - 554.0f - 5.0f * kFilterGap) / 6.0f;
constexpr float kInstalledWidth = 190.0f; // Genre, Tags and Hide installed, left of Saved filters
constexpr int kReset = 99;                 // filter_rect()'s Reset filters
constexpr float kGridTop = 390.0f;
constexpr float kKeyboardTop = 560.0f;
constexpr const char *kRegions[] = {"", "EUR", "USA", "JPN", "ASIA", "unknown"};
constexpr const char *kChips[] = {/* i18n */ "All games", /* i18n */ "Favourites"};

// Removes the last UTF-8 character.
void pop_character(std::string &text)
{
    while (!text.empty())
    {
        const unsigned char c = static_cast<unsigned char>(text.back());
        text.pop_back();
        if ((c & 0xC0) != 0x80)
            break;
    }
}

float chip_width(const ui::Type &type, int chip)
{
    (void)type;
    (void)chip;
    return 185.0f;
}

} // namespace

BrowseScreen::BrowseScreen(Context &context, bool addons) : Screen(context), addons_(addons)
{
    xb::keyboard_look(keyboard_.style, false);
    keyboard_.style.bindings = hui::ui::KeyboardBindings::standard();
    keyboard_.style.max_length = 40;
    keyboard_.style.done_label = tr("Done");
    keyboard_.set_bounds({ui::kGutter + 220.0f, kKeyboardTop + 40.0f, 1280.0f, 400.0f});
}

void BrowseScreen::retranslate()
{
    keyboard_.style.done_label = tr("Done");
}

void BrowseScreen::refilter()
{
    const Snapshot &state = ctx_.store.state();
    genre_choices_ = addons_ ? std::vector<std::string>{} : catalogue_genres(catalogue_games());
    catalog_serial_ = state.catalog_serial;
    favourites_serial_ = state.serial;
    filters_.torbox_sources = state.debrid.sources();
    torbox_offered_ = state.debrid.connected;
    std::vector<int> results = browse(catalogue_games(), filters_, state.favourites, state.library_games());
    if (addons_ && filters_.sort == Sort::release) std::stable_sort(results.begin(), results.end(), [this](int a, int b) {
        const auto &x=catalogue_games()[a].releases.front(), &y=catalogue_games()[b].releases.front();
        return x.added_at > y.added_at;
    });
    // Favourites and Hide installed refilter on every store change: the same
    // results keep their focus rings rather than fading them out and in.
    if (results != results_ || tile_focus_.size() != results.size())
        tile_focus_.assign(results.size(), 0.0f);
    results_ = std::move(results);
    index_ = std::clamp(index_, 0, std::max(0, static_cast<int>(results_.size()) - 1));
    count_choices();
}

void BrowseScreen::count_choices()
{
    const Snapshot &state = ctx_.store.state();
    const std::vector<Game> &games = catalogue_games();
    const auto count = [&](const auto &choose)
    {
        BrowseFilters filters = filters_;
        choose(filters);
        return browse_count(games, filters, state.favourites, state.library_games());
    };
    for (std::vector<long> &counts : counts_)
        counts.clear();
    // A multiple choice counts each choice on its own; the first is its "All …".
    counts_[0].push_back(count([](BrowseFilters &f) { f.sources.clear(); }));
    for (const auto &source : source_options())
        counts_[0].push_back(count([&](BrowseFilters &f) { f.sources = {source.first}; }));
    if (addons_)
        for (const char *type : {"", "expansion", "content-pack", "activation", "bundle"})
            counts_[1].push_back(count([&](BrowseFilters &f) { f.addon_type = type; }));
    else
    {
        counts_[1].push_back(count([](BrowseFilters &f) { f.formats.clear(); }));
        for (const std::string &format : format_choices(games))
            counts_[1].push_back(count([&](BrowseFilters &f) { f.formats = {format}; }));
    }
    for (const char *region : kRegions)
        counts_[2].push_back(count([&](BrowseFilters &f) { f.region = region; }));
    for (int size = 0; size < static_cast<int>(SizeFilter::count); ++size)
        counts_[3].push_back(count([&](BrowseFilters &f) { f.size = static_cast<SizeFilter>(size); }));
    for (const char *platform : {"", "ps5", "ps4"})
        counts_[5].push_back(count([&](BrowseFilters &f) { f.platform = platform; }));
    for (const bool hide : {false, true})
        counts_[6].push_back(count([&](BrowseFilters &f) { f.hide_installed = hide; }));
    counts_[7].push_back(count([](BrowseFilters &f) { f.tags.clear(); }));
    for (const std::string &tag : catalogue_tags(games))
        counts_[7].push_back(count([&](BrowseFilters &f) { f.tags = {tag}; }));
    for(const char *value:{"","60","70","80","90"})counts_[9].push_back(count([&](BrowseFilters &f){f.minimum_score=value;}));
    for(const char *value:{"","under5","5to10","10to20","20to40","40plus"})counts_[10].push_back(count([&](BrowseFilters &f){f.playtime_range=value;}));
    for(const char *value:{"main","extras","completionist"})counts_[11].push_back(count([&](BrowseFilters &f){f.play_style=value;}));
    counts_[8].push_back(count([](BrowseFilters &f) { f.genres.clear(); }));
    for (const std::string &genre : genre_choices_)
        counts_[8].push_back(count([&](BrowseFilters &f) { f.genres = {genre}; }));
}

std::vector<std::pair<std::string, std::string>> BrowseScreen::source_options() const
{
    auto choices = source_choices(catalogue_games());
    if (ctx_.store.state().debrid.connected || contains(filters_.sources, kTorboxSource))
        choices.emplace_back(kTorboxSource, tr("TorBox available"));
    return choices;
}

std::vector<std::string> BrowseScreen::counted(int filter, std::vector<std::string> options) const
{
    if (filter < 0 || filter >= static_cast<int>(counts_.size()))
        return options;
    const std::vector<long> &counts = counts_[static_cast<std::size_t>(filter)];
    for (std::size_t i = 0; i < options.size() && i < counts.size(); ++i)
        options[i] = tr("{option} ({count})", {{"option", options[i]}, {"count", i18n::count(counts[i])}});
    return options;
}

ui::Rect BrowseScreen::tile_rect(int index) const
{
    const int row = index / kColumns;
    const int column = index % kColumns;
    return {ui::kGutter + static_cast<float>(column) * kColumnStep,
            kGridTop + static_cast<float>(row) * (addons_ ? kRowStep + 56 : kRowStep), kTile, kTile};
}

void BrowseScreen::keep_filters_in_range()
{
    const std::vector<int> order = filter_order();
    const int filters = static_cast<int>(order.size());
    const auto at = std::find(order.begin(), order.end(), open_filter_);
    if (open_filter_ == kSavedFilters)
    {
        // Saved filters: the row after Reset filters.
        xb_filter_ = filters + 1;
        if (cn_values_)
            cn_row_ = filters + 1;
        if (pn_picker_ == 2)
            pn_row_ = filters + 1;
    }
    else if (at == order.end())
    {
        xb_values_.open = false;
        cn_values_ = false;
        if (pn_picker_ == 2)
            pn_picker_ = 1;
        open_filter_ = -1;
    }
    else
    {
        const int row = static_cast<int>(at - order.begin());
        xb_filter_ = row;
        if (cn_values_)
            cn_row_ = row;
        if (pn_picker_ == 2)
            pn_row_ = row;
    }
    cn_row_ = std::clamp(cn_row_, 0, filters + 2); // the filters, Reset filters, Saved filters, Search
    pn_row_ = std::clamp(pn_row_, 0, filters + 1); // the filters, Reset filters, Saved filters
}

std::vector<int> BrowseScreen::store_header() const
{
    if (addons_)
        return {};
    std::vector<int> out;
    if (!genre_choices_.empty())
        out.push_back(8);
    if (!catalogue_tags(catalogue_games()).empty())
        out.push_back(7);
    out.insert(out.end(),{6,9,10,11});
    return out;
}

int BrowseScreen::filter_columns() const
{
    return 6 + static_cast<int>(store_header().size());
}

int BrowseScreen::column_filter(int column) const
{
    if (column < 6)
        return column;
    const std::vector<int> header = store_header();
    if (column - 6 < static_cast<int>(header.size()))
        return header[static_cast<std::size_t>(column - 6)];
    return resettable() && column == filter_columns() ? -1 : kSavedFilters;
}

int BrowseScreen::store_columns() const
{
    return filter_columns() + (resettable() ? 1 : 0) + 1;
}

// By filter: the six dropdowns, the header's Tags and Hide installed, Saved
// filters, or kReset after it.
ui::Rect BrowseScreen::filter_rect(int index) const
{
    if (index == kSavedFilters)
        return {970.0f, 134.0f, kResetWidth, 44.0f};
    if (index == kReset)
        return {970.0f + kResetWidth + 16.0f, 134.0f, kResetWidth, 44.0f};
    if (index >= 9 && index <= 11) return {ui::kGutter+(index-9)*580.0f,296.0f,550.0f,58.0f};
    if (index >= 6)
    {
        const std::vector<int> header = store_header();
        const auto at = std::find(header.begin(), header.end(), index);
        const float slots = static_cast<float>(std::count_if(at,header.end(),[](int v){return v<9;}));
        return {970.0f - slots * (kInstalledWidth + 16.0f), 134.0f, kInstalledWidth, 44.0f};
    }
    return {554.0f + static_cast<float>(index) * (kFilterWidth + kFilterGap), kFiltersY,
            kFilterWidth, ui::kFieldHeight};
}

std::vector<int> BrowseScreen::filter_order() const
{
    if (addons_)
        return {5, 0, 1, 2, 3, 4};
    std::vector<int> out{5, 0, 1};
    if (!genre_choices_.empty()) out.push_back(8);
    if (!catalogue_tags(catalogue_games()).empty()) out.push_back(7);
    out.insert(out.end(), {6,9,10,11,2,3,4});
    return out;
}

std::vector<const SavedFilter *> BrowseScreen::saved_here() const
{
    std::vector<const SavedFilter *> out;
    for (const SavedFilter &saved : ctx_.store.state().saved_filters)
        if (saved.addons == addons_)
            out.push_back(&saved);
    std::sort(out.begin(), out.end(), [](const SavedFilter *a, const SavedFilter *b) { return a->name < b->name; });
    return out;
}

void BrowseScreen::apply_saved(const SavedFilter &saved)
{
    const std::string base = filters_.addon_base;
    filters_ = saved.filters;
    filters_.addon_base = base;
    index_ = 0;
    refilter();
}

void BrowseScreen::start_naming()
{
    naming_ = true;
    saved_name_.clear();
    keyboard_open_ = true;
    keyboard_.enter();
    keyboard_.set_length(0);
}

bool BrowseScreen::delete_saved(int option, Feedback &feedback)
{
    const std::vector<const SavedFilter *> saved = saved_here();
    if (option < 1 || option > static_cast<int>(saved.size()) || ctx_.store.saved_filters_action().busy)
        return false;
    ctx_.store.change_saved_filters("{\"action\":\"delete\",\"id\":" + json::quote(saved[static_cast<std::size_t>(option - 1)]->id) + "}");
    feedback.play(hui::audio::Cue::erase);
    return true;
}

std::vector<std::string> BrowseScreen::filter_options(int filter, int *selected, std::vector<bool> *checked) const
{
    const std::vector<Game> &games = catalogue_games();
    std::vector<std::string> options;
    *selected = 0;
    if (checked != nullptr)
        checked->clear();
    if (filter == kSavedFilters)
    {
        *selected = -1; // actions, not a choice that is current
        options.push_back(tr("Save these filters"));
        for (const SavedFilter *saved : saved_here())
            options.push_back(saved->name);
        return options;
    }
    if (addons_ && filter == 1) {
        const std::vector<std::string> ids{"", "expansion", "content-pack", "activation", "bundle"};
        options.push_back(tr("All types"));
        for (int i=1; i<5; ++i) { options.push_back(addon_type_label(ids[i])); if (ids[i]==filters_.addon_type) *selected=i; }
        return options;
    }
    switch (filter)
    {
    case 0:
    {
        options.push_back(tr("All sources"));
        if (checked != nullptr)
            checked->push_back(filters_.sources.empty());
        const auto sources = source_options();
        for (std::size_t i = 0; i < sources.size(); ++i)
        {
            options.push_back(sources[i].second);
            const bool on = std::find(filters_.sources.begin(), filters_.sources.end(), sources[i].first) != filters_.sources.end();
            if (checked != nullptr)
                checked->push_back(on);
            if (on && *selected == 0)
                *selected = static_cast<int>(i) + 1;
        }
        break;
    }
    case 1:
    {
        options.push_back(tr("All formats"));
        if (checked != nullptr)
            checked->push_back(filters_.formats.empty());
        const auto formats = format_choices(games);
        for (std::size_t i = 0; i < formats.size(); ++i)
        {
            options.push_back(formats[i]);
            const bool on = std::find(filters_.formats.begin(), filters_.formats.end(), formats[i]) != filters_.formats.end();
            if (checked != nullptr)
                checked->push_back(on);
            if (on && *selected == 0)
                *selected = static_cast<int>(i) + 1;
        }
        break;
    }
    case 2:
        options = {tr("All regions"), "EUR", "USA", "JPN", "ASIA", tr("Unknown region")};
        for (int i = 0; i < 6; ++i)
            if (filters_.region == kRegions[i])
                *selected = i;
        break;
    case 3:
        for (int i = 0; i < static_cast<int>(SizeFilter::count); ++i)
            options.push_back(size_label(static_cast<SizeFilter>(i)));
        *selected = static_cast<int>(filters_.size);
        break;
    case 5:
        options = {tr("All platforms"), "PS5", "PS4"};
        *selected = filters_.platform == "ps4" ? 2 : filters_.platform == "ps5" ? 1 : 0;
        break;
    case 9:
        options={tr("Any score"),"60+","70+","80+","90+"};
        *selected=filters_.minimum_score.empty()?0:filters_.minimum_score=="60"?1:filters_.minimum_score=="70"?2:filters_.minimum_score=="80"?3:4; break;
    case 10:
        options={tr("Any playtime"),tr("Under 5 hours"),tr("5–10 hours"),tr("10–20 hours"),tr("20–40 hours"),tr("40+ hours")};
        {const char *v[]={"","under5","5to10","10to20","20to40","40plus"};for(int i=0;i<6;++i)if(filters_.playtime_range==v[i])*selected=i;}break;
    case 11:
        options={tr("Main Story"),tr("Main + Extras"),tr("Completionist")};*selected=filters_.play_style=="extras"?1:filters_.play_style=="completionist"?2:0;break;
    case 6:
        options = {tr("Show installed"), tr("Hide installed")};
        *selected = filters_.hide_installed ? 1 : 0;
        break;
    case 7:
    case 8:
    {
        const auto &selected_values = filter == 8 ? filters_.genres : filters_.tags;
        options.push_back(tr(filter == 8 ? "All genres" : "All tags"));
        if (checked != nullptr)
            checked->push_back(selected_values.empty());
        const std::vector<std::string> tags = filter == 8 ? genre_choices_ : catalogue_tags(games);
        for (std::size_t i = 0; i < tags.size(); ++i)
        {
            options.push_back(tr(filter == 8 ? tags[i].c_str() : tag_label(tags[i])));
            const bool on = std::find(selected_values.begin(), selected_values.end(), tags[i]) != selected_values.end();
            if (checked != nullptr)
                checked->push_back(on);
            if (on && *selected == 0)
                *selected = static_cast<int>(i) + 1;
        }
        break;
    }
    default:
        for (int i = 0; i < static_cast<int>(Sort::count); ++i)
            options.push_back(addons_ && i == static_cast<int>(Sort::release) ? tr("Recently added") : sort_label(static_cast<Sort>(i)));
        *selected = static_cast<int>(filters_.sort);
        break;
    }
    return options;
}

std::string BrowseScreen::filter_value(int filter) const
{
    if (filter == kSavedFilters)
    {
        const long count = static_cast<long>(saved_here().size());
        return count ? trn(count, "{count} saved", "{count} saved") : std::string(tr("None yet"));
    }
    int selected = 0;
    std::vector<bool> checked;
    const std::vector<std::string> options = filter_options(filter, &selected, &checked);
    if (multiple(filter) && !checked.empty() && !checked[0])
    {
        // The ticked ones, as the browser's Select lists them; a saved filter's
        // values this catalogue doesn't offer, as they were saved.
        std::string out;
        for (std::size_t i = 1; i < options.size() && i < checked.size(); ++i)
            if (checked[i])
                out += (out.empty() ? "" : ", ") + options[i];
        if (out.empty())
            for (const std::string &value : filter == 0 ? filters_.sources : filter == 1 ? filters_.formats : filter == 8 ? filters_.genres : filters_.tags)
                out += (out.empty() ? "" : ", ") + value;
        return out;
    }
    return selected >= 0 && selected < static_cast<int>(options.size())
               ? options[static_cast<std::size_t>(selected)]
               : std::string();
}

void BrowseScreen::choose(int filter, int option)
{
    if (filter == kSavedFilters)
    {
        const std::vector<const SavedFilter *> saved = saved_here();
        if (option == 0)
            start_naming();
        else if (option <= static_cast<int>(saved.size()))
            apply_saved(*saved[static_cast<std::size_t>(option - 1)]);
        return;
    }
    const std::vector<Game> &games = catalogue_games();
    if (addons_ && filter == 1) {
        const std::vector<std::string> ids{"", "expansion", "content-pack", "activation", "bundle"};
        filters_.addon_type = ids[std::clamp(option,0,4)]; refilter(); return;
    }
    switch (filter)
    {
    case 0:
    case 1:
    case 7:
    case 8:
    {
        // Ticks or unticks one; the first choice (All …) clears them.
        std::vector<std::string> ids;
        if (filter == 0)
            for (const auto &source : source_options())
                ids.push_back(source.first);
        else
            ids = filter == 1 ? format_choices(games) : filter == 8 ? genre_choices_ : catalogue_tags(games);
        std::vector<std::string> &list = filter == 0 ? filters_.sources : filter == 1 ? filters_.formats : filter == 8 ? filters_.genres : filters_.tags;
        if (option <= 0 || option > static_cast<int>(ids.size()))
            list.clear();
        else
        {
            const std::string &id = ids[static_cast<std::size_t>(option - 1)];
            const auto at = std::find(list.begin(), list.end(), id);
            if (at == list.end())
                list.push_back(id);
            else
                list.erase(at);
        }
        break;
    }
    case 2:
        filters_.region = kRegions[std::clamp(option, 0, 5)];
        break;
    case 3:
        filters_.size =
            static_cast<SizeFilter>(std::clamp(option, 0, static_cast<int>(SizeFilter::count) - 1));
        break;
    case 5:
        filters_.platform = option == 2 ? "ps4" : option == 1 ? "ps5" : "";
        break;
    case 9: {const char *v[]={"","60","70","80","90"};filters_.minimum_score=v[std::clamp(option,0,4)];break;}
    case 10: {const char *v[]={"","under5","5to10","10to20","20to40","40plus"};filters_.playtime_range=v[std::clamp(option,0,5)];break;}
    case 11: {const char *v[]={"main","extras","completionist"};filters_.play_style=v[std::clamp(option,0,2)];break;}
    case 6:
        filters_.hide_installed = option == 1;
        break;
    default:
        filters_.sort = static_cast<Sort>(std::clamp(option, 0, static_cast<int>(Sort::count) - 1));
        break;
    }
    index_ = 0;
    refilter();
}

void BrowseScreen::focus()
{
    search_mode_ = false;
    if (catalog_serial_ != ctx_.store.state().catalog_serial)
        refilter();
}

void BrowseScreen::open_search_section(bool keyboard)
{
    search_mode_ = true;
    if (keyboard)
    {
        start_search();
        return;
    }
    // Reached by moving, the keyboard stays closed: opening it on arrival turned
    // the Cross pressed to open it into a typed letter, and in Crossbar kept
    // left/right from moving on.
    area_ = Area::field;
    keyboard_open_ = false;
    xb_.index = 0;
    xb_.snap();
}

void BrowseScreen::start_search()
{
    area_ = Area::field;
    keyboard_open_ = true;
    keyboard_.enter();
    keyboard_.set_length(static_cast<int>(filters_.query.size()));
}

void BrowseScreen::set_query(const std::string &query)
{
    filters_.query = query;
    index_ = 0;
    refilter();
    keyboard_.set_length(static_cast<int>(filters_.query.size()));
}

void BrowseScreen::show_saved_filters()
{
    const int filters = static_cast<int>(filter_order().size());
    open_filter_ = kSavedFilters;
    picking_ = kSavedFilters;
    if (look::is(look::Interface::crossbar))
    {
        xb_filters_ = true;
        xb_filter_ = filters + 1;
        xb_menu_.show(filters + 1);
        xb_values_.show(0);
    }
    else if (look::is(look::Interface::cinema))
    {
        cn_sheet_ = true;
        cn_row_ = filters + 1;
        cn_values_ = true;
        cn_value_ = 0;
    }
    else if (look::is(look::Interface::panorama))
    {
        pn_row_ = filters + 1;
        pn_picker_ = 2;
        pn_value_ = 0;
    }
    else
    {
        area_ = Area::filters;
        column_ = store_columns() - 1;
        picker_.show(0);
    }
}

void BrowseScreen::return_to(const std::string &game_id)
{
    if (look::is(look::Interface::panorama))
    {
        // Back on the tile the game was opened from.
        const pano::Page page = panorama_page();
        const std::string here = pano_.focused_key(page);
        if (here.size() > 2 && here.compare(2, std::string::npos, game_id) == 0)
            return;
        if (!pano_.focus_key(page, "r:" + game_id))
            pano_.focus_key(page, "f:" + game_id);
        return;
    }
    const std::vector<Game> &games = catalogue_games();
    for (int i = 0; i < static_cast<int>(results_.size()); ++i)
    {
        if (games[static_cast<std::size_t>(results_[static_cast<std::size_t>(i)])].id == game_id)
        {
            index_ = i;
            area_ = Area::grid;
            return;
        }
    }
}

void BrowseScreen::update_keyboard(const InputFrame &input, Feedback &feedback)
{
    const hui::ui::Event event = keyboard_.handle(input, feedback);
    if (naming_)
    {
        // Naming the filters: Done saves them under the name; Close doesn't.
        for (int i = 0; i < keyboard_.erased(); ++i)
            pop_character(saved_name_);
        saved_name_ += keyboard_.typed();
        if (event == hui::ui::Event::activated || event == hui::ui::Event::cancelled)
        {
            const std::size_t first = saved_name_.find_first_not_of(' ');
            const std::string name = first == std::string::npos ? std::string()
                                     : saved_name_.substr(first, saved_name_.find_last_not_of(' ') - first + 1);
            if (event == hui::ui::Event::activated && !name.empty())
                ctx_.store.change_saved_filters(saved_filter_body(name, addons_, filters_));
            keyboard_open_ = false;
            naming_ = false;
            feedback.play(event == hui::ui::Event::activated && !name.empty() ? hui::audio::Cue::select
                                                                              : hui::audio::Cue::modal_close);
        }
        return;
    }
    bool changed = false;
    for (int i = 0; i < keyboard_.erased(); ++i)
    {
        pop_character(filters_.query);
        changed = true;
    }
    if (!keyboard_.typed().empty())
    {
        filters_.query += keyboard_.typed();
        changed = true;
    }
    if (changed)
    {
        index_ = 0;
        refilter();
    }
    if (event == hui::ui::Event::activated || event == hui::ui::Event::cancelled)
    {
        keyboard_open_ = false;
        // Done goes to the results; back returns to the field.
        area_ = event == hui::ui::Event::activated && !results_.empty() ? Area::grid : Area::field;
        feedback.play(hui::audio::Cue::modal_close);
    }
}

Intent BrowseScreen::update(const InputFrame &input, float dt, Feedback &feedback, bool focused)
{
    Intent intent;
    bool cut = false; // an L2 / R2 jump moves the page without sliding
    const Snapshot &state = ctx_.store.state();
    if (catalog_serial_ != state.catalog_serial || torbox_changed(state) ||
        ((filters_.favourites || filters_.hide_installed) && favourites_serial_ != state.serial))
        refilter();

    xb::keyboard_look(keyboard_.style, look::is(look::Interface::crossbar));
    keep_filters_in_range();
    if (look::is(look::Interface::crossbar))
        return update_crossbar(input, dt, feedback, focused);
    if (look::is(look::Interface::cinema))
        return update_cinema(input, dt, feedback, focused);
    if (look::is(look::Interface::panorama))
        return update_panorama(input, dt, feedback, focused);
    if (focused && keyboard_open_)
        update_keyboard(input, feedback);
    else if (focused && picker_.open && picking_ == kSavedFilters && input.is_pressed(Action::west))
    {
        if (delete_saved(picker_.active, feedback))
            picker_.active = std::max(0, std::min(picker_.active, static_cast<int>(saved_here().size()) - 1));
        else
            refuse(feedback, input);
    }
    else if (focused && picker_.open && picking_ == kSavedFilters && picker_.active == 0 &&
             input.is_pressed(Action::confirm) && !resettable())
        refuse(feedback, input); // nothing set to save
    else if (focused && picker_.open)
    {
        int selected = 0;
        const int count = static_cast<int>(filter_options(picking_, &selected).size());
        const int chosen = picker_.update(input, count, feedback);
        if (chosen >= 0)
        {
            choose(picking_, chosen);
            // Reset filters may appear before Saved filters: the focus stays on it.
            if (picking_ == kSavedFilters)
                column_ = store_columns() - 1;
            // Source and Format stay open for the next tick; Circle closes them.
            if (multiple(picking_))
                picker_.show(chosen);
        }
    }
    else if (focused)
    {
        const auto move_to = [&](Area area, int column)
        {
            area_ = area;
            column_ = column;
            feedback.play(hui::audio::Cue::focus);
        };
        switch (area_)
        {
        case Area::field:
            if (input.nav == Direction::up)
            {
                intent.kind = Intent::Kind::top_bar;
                return intent;
            }
            if (input.nav == Direction::down)
                move_to(Area::filters, 2);
            else if (input.nav != Direction::none)
                refuse(feedback, input);
            if (input.is_pressed(Action::confirm))
            {
                feedback.play(hui::audio::Cue::modal_open);
                start_search();
            }
            else if (input.is_pressed(Action::west) && !filters_.query.empty())
            {
                filters_.query.clear();
                refilter();
                feedback.play(hui::audio::Cue::erase);
            }
            break;
        case Area::collection:
        case Area::filters:
        {
            const int count = area_ == Area::collection ? (addons_ ? 1 : 2) : store_columns();
            if (input.nav == Direction::up)
                move_to(Area::field, 0);
            else if (input.nav == Direction::down)
            {
                if (!results_.empty())
                {
                    area_ = Area::grid;
                    index_ = std::min(index_, kColumns - 1);
                    feedback.play(hui::audio::Cue::focus);
                }
                else
                    refuse(feedback, input);
            }
            else if (input.nav == Direction::left || input.nav == Direction::right)
            {
                const int next = column_ + (input.nav == Direction::right ? 1 : -1);
                if (next >= 0 && next < count)
                    move_to(area_, next);
                else if (area_ == Area::collection && next == count)
                    move_to(Area::filters, 0);
                else if (area_ == Area::filters && next < 0)
                    move_to(Area::collection, addons_ ? 0 : 1);
                else
                    refuse(feedback, input);
            }
            if (input.is_pressed(Action::confirm))
            {
                if (area_ == Area::collection)
                {
                    const bool favourites = !addons_ && column_ == 1;
                    if (addons_) filters_.addon_base.clear();
                    if (favourites && !state.paired)
                    {
                        refuse(feedback, input);
                    }
                    else
                    {
                        filters_.favourites = favourites;
                        index_ = 0;
                        refilter();
                        feedback.play(hui::audio::Cue::toggle);
                    }
                }
                else if (column_filter(column_) == 6)
                {
                    // A button rather than a dropdown: there is no room for a seventh.
                    filters_.hide_installed = !filters_.hide_installed;
                    index_ = 0;
                    refilter();
                    feedback.play(hui::audio::Cue::toggle);
                }
                else if (column_filter(column_) < 0)
                {
                    filters_ = BrowseFilters{};
                    index_ = 0;
                    refilter();
                    feedback.play(hui::audio::Cue::toggle);
                }
                else if (column_filter(column_) == kSavedFilters && !state.paired)
                    refuse(feedback, input);
                else
                {
                    int selected = 0;
                    picking_ = column_filter(column_);
                    filter_options(picking_, &selected);
                    picker_.show(selected);
                    feedback.play(hui::audio::Cue::modal_open);
                }
            }
            break;
        }
        case Area::grid:
        {
            const int count = static_cast<int>(results_.size());
            if (count == 0)
            {
                area_ = Area::filters;
                break;
            }
            if (input.is_pressed(Action::jump_prev) || input.is_pressed(Action::jump_next))
            {
                const int jump = grid_jump(index_, count, kColumns,
                                           input.is_pressed(Action::jump_next) ? kGridJumpRows : -kGridJumpRows);
                if (jump != index_)
                {
                    index_ = jump;
                    cut = true;
                    feedback.play(hui::audio::Cue::focus);
                }
                else
                    refuse(feedback, input);
            }
            int next = index_;
            if (input.nav == Direction::left)
                next = index_ % kColumns == 0 ? -1 : index_ - 1;
            else if (input.nav == Direction::right)
                next = index_ % kColumns == kColumns - 1 || index_ + 1 >= count ? -1 : index_ + 1;
            else if (input.nav == Direction::down)
                next = index_ + kColumns < count                    ? index_ + kColumns
                       : index_ / kColumns < (count - 1) / kColumns ? count - 1
                                                                    : -1;
            else if (input.nav == Direction::up)
                next = index_ - kColumns;
            if (input.nav != Direction::none)
            {
                if (input.nav == Direction::up && next < 0)
                {
                    move_to(index_ % kColumns < 2 ? Area::collection : Area::filters,
                            index_ % kColumns < 2 ? index_ % kColumns : index_ % kColumns - 2);
                }
                else if (next >= 0 && next < count)
                {
                    index_ = next;
                    feedback.play(hui::audio::Cue::focus, 1.0f, 0.0f, 0.8f);
                }
                else
                {
                    refuse(feedback, input);
                }
            }
            if (area_ == Area::grid && input.is_pressed(Action::west))
            {
                const Game &game = catalogue_games()[static_cast<std::size_t>(
                    results_[static_cast<std::size_t>(index_)])];
                if (!addons_ && state.paired && state.link == Link::online &&
                    !ctx_.store.favourite_action().busy)
                {
                    ctx_.store.set_favourite(game.id, !state.favourite(game.id));
                    feedback.play(hui::audio::Cue::toggle);
                }
                else
                    refuse(feedback, input);
            }
            else if (input.is_pressed(Action::confirm))
            {
                feedback.play(hui::audio::Cue::select);
                intent.kind = Intent::Kind::open_game;
                intent.id = catalogue_games()[static_cast<std::size_t>(
                    results_[static_cast<std::size_t>(index_)])].id;
                return intent;
            }
            if (input.is_pressed(Action::north))
            {
                area_ = Area::field;
                feedback.play(hui::audio::Cue::modal_open);
                start_search();
            }
            break;
        }
        }
    }

    // Animation.
    keyboard_.set_active(keyboard_open_);
    keyboard_.update(dt);
    keyboard_in_.target = keyboard_open_ ? 1.0f : 0.0f;
    keyboard_in_.update(dt, 14.0f);
    picker_.animate(dt);
    const bool live = focused && !keyboard_open_ && !picker_.open;
    field_focus_.target = focused && area_ == Area::field ? 1.0f : 0.0f;
    field_focus_.update(dt, 16.0f);
    for (int i = 0; i < 2 + kFilterControls; ++i)
    {
        const bool on = live && ((area_ == Area::collection && i == column_) ||
                                 (area_ == Area::filters && i == 2 + column_));
        control_focus_[i] += ((on ? 1.0f : 0.0f) - control_focus_[i]) * std::min(1.0f, dt * 16.0f);
    }
    // The page scrolls under the top bar to keep the focused row in view.
    float target = 0.0f;
    if (area_ == Area::grid)
    {
        const float step = addons_ ? kRowStep + 56 : kRowStep;
        const float top = tile_rect(index_).y;
        target = std::max(0.0f, top + step - 1010.0f);
        target = std::min(target, std::max(0.0f, top - 140.0f));
    }
    scroll_.target = target;
    if (cut)
        scroll_.snap(target);
    scroll_.update(dt, 10.0f);
    for (int i = 0; i < static_cast<int>(tile_focus_.size()); ++i)
    {
        const bool on = live && area_ == Area::grid && i == index_;
        float &amount = tile_focus_[static_cast<std::size_t>(i)];
        amount += ((on ? 1.0f : 0.0f) - amount) * std::min(1.0f, dt * 16.0f);
    }
    return intent;
}

void BrowseScreen::draw(Frame &frame, bool focused) const
{
    if (look::is(look::Interface::crossbar))
    {
        draw_crossbar(frame);
        return;
    }
    if (look::is(look::Interface::cinema))
    {
        draw_cinema(frame);
        return;
    }
    if (look::is(look::Interface::panorama))
    {
        draw_panorama(frame);
        return;
    }
    (void)focused;
    hui::gfx::DrawList &list = frame.scene;
    const Snapshot &state = ctx_.store.state();
    const ui::Type &type = ctx_.type;
    const float scroll = scroll_.value;

    list.push_clip({0.0f, 104.0f, 1920.0f, 976.0f});
    list.push_transform(1.0f, 0.0f, 0.0f, 0.0f, -scroll);

    // .page-heading: the title and how many games match.
    const std::vector<int> header = store_header();
    const float heading_width = filter_rect(header.empty() ? kSavedFilters : header.front()).x - ui::kGutter - 20.0f;
    ui::text_fit(list, type.semibold, tr(addons_ ? "Add-ons" : "Browse"), ui::kGutter, kTitleBaseline, 60.0f,
                 heading_width, ui::color::text);
    const std::string count =
        trn(static_cast<long>(results_.size()), addons_ ? "{count} add-on" : "{count} game", addons_ ? "{count} add-ons" : "{count} games");
    ui::text_fit(list, type.regular, count, ui::kGutter, kTitleBaseline + 25.0f, 20.0f,
                 heading_width, ui::color::text3);

    // .search-field: a tonal pill, brighter while typing.
    const ui::Rect field{1920.0f - ui::kGutter - kFieldWidth, kFieldY, kFieldWidth, kFieldHeight};
    ui::focus_ring(list, field, field.h * 0.5f, field_focus_.value);
    ui::surface(list, field, field.h * 0.5f,
                keyboard_open_ ? ui::color::field_open : ui::color::field);
    ui::icon_search(list, field.x + 34.0f, field.cy(), 24.0f, ui::color::text2);
    if (filters_.query.empty())
    {
        ui::text(list, type.regular, tr(addons_ ? "Search add-ons or base games" : "Search games or title ID"), field.x + 60.0f, field.cy() + 7.0f,
                 20.0f, ui::color::text3);
    }
    else
    {
        const float w = ui::text_fit(list, type.regular, filters_.query, field.x + 60.0f,
                                     field.cy() + 7.0f, 20.0f, field.w - 90.0f, ui::color::text);
        if (keyboard_open_ && static_cast<int>(ctx_.clock * 2.0f) % 2 == 0)
            list.rounded_rect({field.x + 63.0f + w, field.cy() - 12.0f, 2.0f, 24.0f}, 1.0f,
                              ui::color::text);
    }

    // .collection-tabs: All games and Favourites, one tonal track with the chosen side lit.
    ui::surface(list, {ui::kGutter, kChipsY, addons_ ? 185.0f : 370.0f, kChipHeight}, kChipHeight * .5f,
                ui::color::field);
    list.line(512.0f, kChipsY + 8.0f, 512.0f, kChipsY + kChipHeight - 8.0f, 1.0f,
              ui::Color::rgb(0xffffff, 0.1f));
    float x = ui::kGutter;
    for (int c = 0; c < (addons_ ? 1 : 2); ++c)
    {
        const ui::Rect chip{x, kChipsY, chip_width(type, c), kChipHeight};
        const ui::Rect lit = chip.inset(5.0f);
        const bool on = (c == 1) == filters_.favourites;
        ui::focus_ring(list, lit, lit.h * 0.5f, control_focus_[c]);
        if (on)
            list.rounded_rect(lit, lit.h * 0.5f, ui::Color::rgb(0xf4f4f4));

        ui::text(list, type.medium, tr(addons_ ? "All add-ons" : kChips[c]), chip.cx(), chip.cy() + 7.0f, 20.0f,
                 on ? ui::Color::rgb(0x080808) : ui::color::text, ui::Align::center);
        x += chip.w;
    }

    // .browse-selects: six dropdowns; in the header Genre, Tags, Hide installed and Reset filters.
    for (int f = 0; f < 6; ++f)
    {
        const ui::Rect r = filter_rect(f);
        ui::select_field(
            list, type, r,
            f == 4 && filters_.sort == Sort::release ? tr(addons_ ? "Recently added" : "Newest first") : filter_value(f),
            control_focus_[2 + f], picker_.open && picking_ == f, true, ui::FieldLook::pill);
    }
    for (std::size_t k = 0; k < header.size(); ++k)
    {
        const float focus = control_focus_[2 + 6 + k];
        if (header[k] == 6)
            ui::button(list, type, filter_rect(6), filters_.hide_installed ? tr("Show installed") : tr("Hide installed"),
                       ui::ButtonKind::secondary, focus, true, 18.0f);
        else
            ui::select_field(list, type, filter_rect(header[k]), filter_value(header[k]), focus,
                             picker_.open && picking_ == header[k], true, ui::FieldLook::pill);
    }
    const ui::Rect reset = filter_rect(kReset);
    if (resettable())
        ui::button(list, type, reset, tr("Reset filters"), ui::ButtonKind::secondary, control_focus_[2 + filter_columns()],
                   true, 18.0f);
    ui::button(list, type, filter_rect(kSavedFilters), tr("Saved filters"), ui::ButtonKind::secondary,
               control_focus_[2 + store_columns() - 1], state.paired, 18.0f);
    if (filters_.sort == Sort::size || filters_.sort == Sort::size_desc)
        ui::text(list, type.regular,
                 tr("Sorted by the smallest download option that matches your filters."), ui::kGutter,
                 kFiltersY + ui::kFieldHeight + 26.0f, 15.0f, ui::color::text3);

    // ---- results ----
    if (results_.empty() && state.have_catalog)
    {
        ui::text(list, type.light, tr(addons_ ? "No add-ons match these filters." : "No games found"), ui::kGutter, kGridTop + 70.0f, 40.0f,
                 ui::color::text);
        ui::text(list, type.regular,
                 filters_.favourites ? tr("Add favourites from a game\xE2\x80\x99s page.")
                                     : tr("Try another name, title ID or filter."),
                 ui::kGutter, kGridTop + 114.0f, 20.0f, ui::color::text2);
    }
    for (int pass = 0; pass < 2; ++pass)
    {
        for (int i = 0; i < static_cast<int>(results_.size()); ++i)
        {
            const ui::Rect r = tile_rect(i);
            if (r.y - scroll > 1080.0f || r.y + (addons_ ? kRowStep + 56 : kRowStep) - scroll < 0.0f)
                continue;
            const float amount = tile_focus_[static_cast<std::size_t>(i)];
            const bool on = amount > 0.5f;
            if ((pass == 0) == on)
                continue; // the focused tile is drawn last, over its neighbours
            const Game &game =
                catalogue_games()[static_cast<std::size_t>(results_[static_cast<std::size_t>(i)])];
            draw_tile(ctx_, list, game, r, amount);
            if (state.favourite(game.id))
            {
                // A filled heart in a dark disc, top right.
                const float scale = 1.0f;
                list.push_transform(scale, r.cx(), r.cy(), 0.0f, 0.0f);
                list.circle(r.x + r.w - 25.0f, r.y + 25.0f, 15.0f, ui::Color::rgb(0x141414, 0.87f));
                icons::draw(list, icons::Icon::heart_fill, r.x + r.w - 25.0f, r.y + 25.0f, 18.0f, ui::Color::rgb(0xffffff));
                list.pop_transform();
            }
            const float caption_bottom = draw_tile_caption(ctx_, list, game,
                              game_state(game, state.jobs, state.library_games()).label, r.x,
                              r.y + r.h, r.w, amount, false);
            if (look::prefs().card_insights && !insight_summary(game.insights).empty()) ui::text_fit(list,type.regular,insight_summary(game.insights),r.x,caption_bottom+20.0f,15,r.w,ui::color::text3);
        }
    }
    list.pop_transform();
    list.pop_clip();

    // ---- a filter's list ----
    if (picker_.in.value > 0.01f)
    {
        int selected = 0;
        std::vector<bool> checked;
        const std::vector<std::string> options = counted(picking_, filter_options(picking_, &selected, &checked));
        std::vector<bool> dimmed;
        if (picking_ >= 0 && picking_ < static_cast<int>(counts_.size()))
            for (long count : counts_[static_cast<std::size_t>(picking_)]) dimmed.push_back(count == 0);
        ui::Rect trigger = filter_rect(picking_);
        trigger.y -= scroll;
        frame.glass = true;
        ui::select_menu(frame.overlay, type, trigger, options, selected, picker_.active,
                        picker_.in.value, frame.glass_texture, multiple(picking_) ? &checked : nullptr, &dimmed);
    }

    // ---- keyboard ----
    if (keyboard_in_.value > 0.01f)
    {
        hui::gfx::DrawList &overlay = frame.overlay;
        frame.glass = true;
        const float in = keyboard_in_.value;
        overlay.push_opacity(in);
        overlay.push_transform(1.0f, 0.0f, 0.0f, 0.0f, 60.0f * (1.0f - in));
        const ui::Rect panel{0.0f, kKeyboardTop, 1920.0f, 1080.0f - kKeyboardTop};
        overlay.glass(frame.glass_texture, panel, 0.0f, ui::Color::rgb(0xffffff));
        overlay.rounded_rect(panel, 0.0f, ui::color::night1.with_alpha(0.78f));
        overlay.rounded_rect({0.0f, kKeyboardTop, 1920.0f, 1.5f}, 0.0f, ui::color::line);
        if (naming_)
        {
            // The name, in a field over the keys (the search field stays as it is).
            const ui::Rect name{ui::kGutter + 220.0f, kKeyboardTop - 92.0f, 1280.0f, kFieldHeight};
            overlay.glass(frame.glass_texture, name, name.h * 0.5f, ui::Color::rgb(0xffffff));
            ui::surface(overlay, name, name.h * 0.5f, ui::color::field_open);
            const float w = ui::text_fit(overlay, type.regular, saved_name_.empty() ? std::string(tr("Name these filters")) : saved_name_,
                                         name.x + 34.0f, name.cy() + 7.0f, 20.0f, name.w - 68.0f,
                                         saved_name_.empty() ? ui::color::text3 : ui::color::text);
            if (static_cast<int>(ctx_.clock * 2.0f) % 2 == 0)
                overlay.rounded_rect({name.x + 37.0f + (saved_name_.empty() ? 0.0f : w), name.cy() - 12.0f, 2.0f, 24.0f}, 1.0f,
                                     ui::color::text);
        }
        hui::ui::Canvas canvas{overlay, ctx_.fonts, frame.glass_texture, ctx_.clock};
        keyboard_.draw(canvas);
        overlay.pop_transform();
        overlay.pop_opacity();
    }
}

std::vector<hui::ui::Hint> BrowseScreen::hints() const
{
    if (look::is(look::Interface::crossbar))
        return crossbar_hints();
    if (look::is(look::Interface::cinema))
        return cinema_hints();
    if (look::is(look::Interface::panorama))
        return panorama_hints();
    if (keyboard_open_)
        return {{hui::ui::Button::square, tr("Delete")},
                {hui::ui::Button::triangle, tr("Add space")},
                {hui::ui::Button::circle, tr("Close")}};
    if (picker_.open && picking_ == kSavedFilters && picker_.active > 0)
        return {{hui::ui::Button::cross, tr("Apply")}, {hui::ui::Button::square, tr("Delete")}, {hui::ui::Button::circle, tr("Close")}};
    if (picker_.open)
        return {{hui::ui::Button::cross, tr("Choose")}, {hui::ui::Button::circle, tr("Close")}};
    if (area_ == Area::grid)
    {
        std::vector<hui::ui::Hint> hints{{hui::ui::Button::cross, tr(addons_ ? "Open" : "Open game")}};
        const Snapshot &state = ctx_.store.state();
        if (!addons_ && state.paired && index_ >= 0 && index_ < static_cast<int>(results_.size()))
        {
            const Game &game = catalogue_games()[static_cast<std::size_t>(
                results_[static_cast<std::size_t>(index_)])];
            hints.push_back({hui::ui::Button::square,
                             state.favourite(game.id) ? tr("Remove from favourites")
                                                      : tr("Add to favourites")});
        }
        hints.push_back({hui::ui::Button::triangle, tr("Search")});
        hints.push_back({hui::ui::Button::l2, tr("Jump"), hui::ui::Button::r2});
        hints.push_back({hui::ui::Button::l1, tr("Tabs"), hui::ui::Button::r1});
        return hints;
    }
    if (area_ == Area::collection || area_ == Area::filters)
        return {{hui::ui::Button::cross, tr("Select")},
                {hui::ui::Button::l1, tr("Tabs"), hui::ui::Button::r1}};
    if (!filters_.query.empty())
        return {{hui::ui::Button::cross, tr("Type text")},
                {hui::ui::Button::square, tr("Clear")},
                {hui::ui::Button::l1, tr("Tabs"), hui::ui::Button::r1}};
    return {{hui::ui::Button::cross, tr("Type text")}, {hui::ui::Button::l1, tr("Tabs"), hui::ui::Button::r1}};
}

} // namespace orbit
