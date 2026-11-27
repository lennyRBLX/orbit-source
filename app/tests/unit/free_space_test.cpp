// Orbit Store TV app - Free up space (free_space_form.cpp): the drive's list as the
// console sends it, and the form from Not enough space to the deletion it follows.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fixture.hpp"
#include "orbit/json.hpp"
#include "orbit/look.hpp"
#include "orbit/model.hpp"
#include "orbit/screens.hpp"
#include "support.hpp"

#include <gtest/gtest.h>

namespace orbit
{
namespace
{

TEST(FreeSpace, ReadsTheDrivesLeftoversGamesAndDeletion)
{
    json::Value value;
    ASSERT_TRUE(json::parse(R"({"storageId":"usb0","label":"USB Extended Storage","freeBytes":30000000000,
        "totalBytes":2000000000000,"projectedFreeBytes":11300000000,
        "leftovers":[{"id":"archive:job7","kind":"archive","title":"Frostline","name":"Frostline.rar","sizeBytes":31000000000},
                     {"id":"","kind":"partial","title":"No ID","sizeBytes":5}],
        "games":[{"id":"game:PPSA90006:k","kind":"game","titleId":"PPSA90006","title":"Quiet Harbour","format":"FFPFSC",
                  "sizeBytes":9800000000,"mounted":true}],
        "deletion":{"state":"deleting","storageId":"usb0","message":"","current":"Frostline","total":2,"done":1,
                    "failed":0,"freedBytes":31000000000}})",
                            &value));
    const SpaceListing listing = parse_space_listing(value);
    EXPECT_EQ(listing.storage_id, "usb0");
    EXPECT_EQ(listing.label, "USB Extended Storage");
    EXPECT_DOUBLE_EQ(listing.projected_free_bytes, 11.3e9);
    ASSERT_EQ(listing.leftovers.size(), 1u); // an item without an ID can't be chosen
    EXPECT_EQ(listing.leftovers[0].kind, "archive");
    EXPECT_EQ(listing.leftovers[0].name, "Frostline.rar");
    ASSERT_EQ(listing.games.size(), 1u);
    EXPECT_TRUE(listing.games[0].mounted);
    EXPECT_EQ(listing.games[0].format, "FFPFSC");
    EXPECT_EQ(listing.deletion.state, "deleting");
    EXPECT_EQ(listing.deletion.current, "Frostline");
    EXPECT_EQ(listing.deletion.done, 1);
    EXPECT_DOUBLE_EQ(listing.deletion.freed_bytes, 31e9);

    // An older service's empty answer reads as nothing running.
    EXPECT_EQ(parse_space_deletion(json::Value{}).state, "idle");
}

struct Harness
{
    test::TestFonts fonts;
    host::Fixture backend{host::FixtureMode::full};
    test::ScriptedTransport transport;
    test::FakeSink sink;
    Store store{transport};
    ArtCache art{transport, sink};
    Context context{fonts.type, fonts.fonts, store, art};
    Feedback feedback;

    Harness()
    {
        look::load("");
        transport.reply = [this](const http::Request &request) { return backend.send(request); };
        store.poll_once();
        store.tick();
    }
    ~Harness()
    {
        look::load("");
    }
    // Runs what the screens asked of the console, then reads it again.
    void settle()
    {
        while (store.run_next_action())
            store.tick();
        store.tick();
    }
    std::vector<std::string> bodies(const std::string &path)
    {
        std::vector<std::string> out;
        for (const http::Request &request : transport.seen)
            if (request.path == path && request.method == "POST")
                out.push_back(request.body);
        return out;
    }
};

InputFrame press(Action action)
{
    InputFrame input;
    input.connected = true;
    input.pressed = hui::action_bit(action);
    return input;
}

InputFrame move(Direction direction)
{
    InputFrame input;
    input.connected = true;
    input.nav = direction;
    return input;
}

TEST(FreeSpace, ShortDownloadNeedsEnoughTickedThenDeletesAndFreesTheDrive)
{
    Harness h;
    FreeSpaceForm form{h.context};
    FreeSpaceForm::Request request;
    request.storage_id = "usb0";
    request.size_bytes = 27.4e9;
    request.need_bytes = 27.4e9 + kSpaceMargin; // 11.3 GB free: 16.1 GB short
    request.then = FreeSpaceForm::Then::download;
    form.open(request);
    const auto frame = [&](const InputFrame &input = InputFrame{})
    {
        const FreeSpaceForm::Result result = form.update(input, 1.0f / 60.0f, h.feedback);
        Frame output;
        form.draw(output);
        return result;
    };
    h.settle();
    frame();
    ASSERT_TRUE(form.active());

    // Not enough space → the list, Orbit's leftovers ticked (12.9 GB of 16.1 GB).
    EXPECT_EQ(frame(press(Action::confirm)), FreeSpaceForm::Result::none);
    EXPECT_EQ(frame(press(Action::confirm)), FreeSpaceForm::Result::none); // too little ticked: refused
    h.settle();
    EXPECT_TRUE(h.bodies("/api/v1/space").size() == 1u) << "only the list was read";

    // Tick Quiet Harbour, the last game, then Delete and download, then confirm.
    frame(move(Direction::up));
    frame(press(Action::confirm));
    frame(move(Direction::down));
    frame(press(Action::confirm)); // the confirmation
    frame(press(Action::confirm)); // Delete and download
    h.settle();
    frame();
    const std::vector<std::string> posts = h.bodies("/api/v1/space");
    ASSERT_EQ(posts.size(), 2u);
    json::Value sent;
    ASSERT_TRUE(json::parse(posts[1], &sent));
    EXPECT_EQ(sent.text("action"), "delete");
    EXPECT_EQ(sent.text("storageId"), "usb0");
    std::vector<std::string> ids;
    for (const json::Value &id : sent["ids"].items())
        ids.push_back(id.as_string());
    EXPECT_EQ(ids, (std::vector<std::string>{"package:c0ffee0000000001:5a17e0000001", "partial:job105",
                                             "game:PPSA90006:usb0-harbour"}));

    // The form follows the deletion, reads the drive again and hands back.
    FreeSpaceForm::Result result = FreeSpaceForm::Result::none;
    for (int i = 0; i < 12 && result == FreeSpaceForm::Result::none; ++i)
    {
        h.store.poll_once();
        h.settle();
        result = frame();
    }
    EXPECT_EQ(result, FreeSpaceForm::Result::freed);
    EXPECT_FALSE(form.active());
    EXPECT_GT(h.transport.count("/api/v1/space"), 3); // polled while deleting
}

TEST(FreeSpace, BackClosesAndNothingIsDeleted)
{
    Harness h;
    FreeSpaceForm form{h.context};
    FreeSpaceForm::Request request;
    request.storage_id = "internal";
    form.open(request); // from Library: the list at once, nothing to start after
    h.settle();
    EXPECT_EQ(form.update(InputFrame{}, 1.0f / 60.0f, h.feedback), FreeSpaceForm::Result::none);
    // No leftovers on this drive and games are never ticked: Delete is refused.
    form.update(press(Action::confirm), 1.0f / 60.0f, h.feedback);
    EXPECT_EQ(form.update(press(Action::back), 1.0f / 60.0f, h.feedback), FreeSpaceForm::Result::closed);
    EXPECT_FALSE(form.active());
    h.settle();
    for (const std::string &body : h.bodies("/api/v1/space"))
        EXPECT_EQ(body.find("\"delete\""), std::string::npos);
}

// The game page: a download that doesn't fit opens Free up space instead of
// downloading, and starts once there's room.
TEST(FreeSpace, GamePageOpensItForADownloadThatDoesNotFitThenDownloads)
{
    Harness h;
    for (int i = 0; i < 3; ++i)
    {
        h.store.poll_once();
        h.settle();
    }
    GamePage page{h.context};
    page.open("echo-valley"); // 27.4 GB; neither drive has the room
    const auto frame = [&](const InputFrame &input = InputFrame{})
    {
        const Intent intent = page.update(input, 1.0f / 60.0f, h.feedback, true);
        Frame output;
        page.draw(output, true);
        return intent;
    };
    frame();
    frame(move(Direction::down));
    frame(move(Direction::down));
    for (int i = 0; i < 4 && h.transport.count("/api/v1/space") == 0; ++i)
    {
        frame(press(Action::confirm));
        h.settle();
    }
    frame();
    ASSERT_EQ(h.bodies("/api/v1/space").size(), 1u) << "Download opened Free up space";
    EXPECT_TRUE(page.modal());
    EXPECT_TRUE(h.bodies("/api/v1/downloads").empty()) << "nothing downloads before there's room";

    // Free up space, tick the last game, Delete and download, confirm.
    frame(press(Action::confirm));
    frame(move(Direction::up));
    frame(press(Action::confirm));
    frame(move(Direction::down));
    frame(press(Action::confirm));
    frame(press(Action::confirm));
    h.settle();
    for (int i = 0; i < 12 && h.bodies("/api/v1/downloads").empty(); ++i)
    {
        h.store.poll_once();
        h.settle();
        frame();
        h.settle();
    }
    const std::vector<std::string> space = h.bodies("/api/v1/space");
    const std::vector<std::string> downloads = h.bodies("/api/v1/downloads");
    ASSERT_GE(space.size(), 2u);
    ASSERT_EQ(downloads.size(), 1u);
    json::Value deleted, download;
    ASSERT_TRUE(json::parse(space[1], &deleted));
    ASSERT_TRUE(json::parse(downloads[0], &download));
    EXPECT_EQ(deleted.text("action"), "delete");
    EXPECT_EQ(download.text("storageId"), deleted.text("storageId")); // on the drive it freed
}

} // namespace
} // namespace orbit
