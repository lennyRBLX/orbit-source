// Orbit Store TV app - Get everything: the rows shared with the browser version
// (src/bundle.ts) and Orbit Zero, and the bundle status it shows.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/model.hpp"
#include "orbit/json.hpp"

#include <gtest/gtest.h>

namespace orbit
{
namespace
{

std::vector<Release> releases(const char *text)
{
    json::Value value;
    EXPECT_TRUE(json::parse(text, &value));
    return parse_catalog(value);
}

// A PS5 game, then its add-ons: one for its title ID, one with an option for another
// region and one for it, one for another game and a PS4 one with the same title ID.
const char *kGame = R"([
 {"id":"ddr-vk","gameId":"ddr","titleId":"PPSA23000","platform":"ps5","title":"Double Dragon",
  "sourceId":"vikingfile","provider":"Vikingfile","format":"FFPFSC","sizeBytes":10600000000,"browserAvailable":true}
])";
const char *kAddons = R"([
 {"id":"colors-0","gameId":"colors","addonId":"colors","titleId":"PPSA23000","platform":"ps5","title":"Character Colors",
  "sourceId":"archive","provider":"Archive.org","format":"PKG","sizeBytes":1400000000,"baseGameId":"ddr"},
 {"id":"pass-jp","gameId":"pass","addonId":"pass","titleId":"PPSA23001","platform":"ps5","title":"Season Pass",
  "sourceId":"archive","provider":"Archive.org","format":"PKG","sizeBytes":1400000000,"baseGameId":"ddr"},
 {"id":"pass-us","gameId":"pass","addonId":"pass","titleId":"PPSA23000","platform":"ps5","title":"Season Pass",
  "sourceId":"archive","provider":"Archive.org","format":"PKG","sizeBytes":1400000000,"baseGameId":"ddr"},
 {"id":"other-0","gameId":"other","addonId":"other","titleId":"PPSA99999","platform":"ps5","title":"Other Pack",
  "sourceId":"archive","provider":"Archive.org","format":"PKG","sizeBytes":1000000000,"baseGameId":"other"},
 {"id":"ps4-0","gameId":"ps4-pack","addonId":"ps4-pack","titleId":"PPSA23000","platform":"ps4","title":"PS4 Pack",
  "sourceId":"archive","provider":"Archive.org","format":"PKG","sizeBytes":1000000000,"baseGameId":"ddr"}
])";

TEST(GetEverything, ListsTheGameThenEachDlcForItsTitleIdAndPlatform)
{
    const std::vector<Game> games = group_games(releases(kGame));
    const std::vector<Game> addons = group_games(releases(kAddons));
    ASSERT_EQ(games.size(), 1u);
    const Release &option = games[0].releases[0];
    std::vector<EverythingRow> rows = everything_rows(games[0], option, addons, {}, "", "");
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0].kind, EverythingRow::Kind::game);
    EXPECT_EQ(rows[0].release->id, "ddr-vk");
    EXPECT_FALSE(rows[0].installed);
    EXPECT_EQ(rows[1].kind, EverythingRow::Kind::dlc);
    EXPECT_EQ(rows[1].release->id, "colors-0");
    EXPECT_EQ(rows[2].release->id, "pass-us"); // the option for this title ID, not the other region's
    EXPECT_TRUE(everything_offered(rows));
    EXPECT_EQ(everything_summary(rows), "Game and 2 DLC");
    EXPECT_EQ(everything_label(rows[0]), "Game");
    EXPECT_EQ(everything_label(rows[1]), "Character Colors (DLC)");

    // Installed already: the game row is marked and left out of the count.
    LibraryGame copy;
    copy.title_id = "PPSA23000";
    copy.platform = "ps5";
    copy.installed = true;
    rows = everything_rows(games[0], option, addons, {copy}, "", "");
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_TRUE(rows[0].installed);
    EXPECT_EQ(everything_summary(rows), "2 DLC");
    // A PS4 copy with the same title ID isn't this game.
    copy.platform = "ps4";
    EXPECT_FALSE(everything_rows(games[0], option, addons, {copy}, "", "")[0].installed);
}

TEST(GetEverything, NotOfferedWithoutDlcOrOnAnAddOnOrWithoutATitleId)
{
    const std::vector<Game> games = group_games(releases(kGame));
    const std::vector<Game> addons = group_games(releases(kAddons));
    const Release &option = games[0].releases[0];
    EXPECT_FALSE(everything_offered(everything_rows(games[0], option, {}, {}, "", "")));
    Release untitled = option;
    untitled.title_id.clear();
    EXPECT_TRUE(everything_rows(games[0], untitled, addons, {}, "", "").empty());
    EXPECT_TRUE(everything_rows(addons[0], addons[0].releases[0], addons, {}, "", "").empty());
}

TEST(GetEverything, BundleStatusReadsItsItems)
{
    json::Value value;
    ASSERT_TRUE(json::parse(R"({"id":"b1","state":"page","items":[
        {"releaseId":"g","kind":"game","state":"added","jobId":"j1"},
        {"releaseId":"d1","kind":"dlc","state":"page"},
        {"releaseId":"d2","kind":"dlc","state":"pending"}]})", &value));
    const Bundle bundle = parse_bundle(value);
    EXPECT_TRUE(bundle.running());
    EXPECT_EQ(bundle.added(), 1);
    ASSERT_NE(bundle.item("d1"), nullptr);
    EXPECT_EQ(std::string(bundle_state_label(bundle.item("d1")->state)), "Open its page on your PS5");
    EXPECT_EQ(std::string(bundle_state_label("skipped")), "Not added");
    EXPECT_EQ(bundle.item("missing"), nullptr);
}

} // namespace
} // namespace orbit
