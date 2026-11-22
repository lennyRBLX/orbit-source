// SPDX-License-Identifier: GPL-3.0-or-later
#include "fixture.hpp"
#include "orbit/screens.hpp"
#include "support.hpp"
#include <gtest/gtest.h>

namespace orbit
{
TEST(QueuePosition, KeepsIdentityAndNextSurvivingNeighbour)
{
    QueuePosition position;
    position.remember("failed", {"a", "b", "c", "d", "e"}, 2);
    EXPECT_EQ(position.restore("failed", {"c", "d", "e"}), 0);
    EXPECT_EQ(position.restore("failed", {"a", "e"}), 1);
    EXPECT_EQ(position.restore("failed", {"a", "b"}), 1);
    EXPECT_EQ(position.restore("failed", {}), -1);
    EXPECT_EQ(position.restore("active", {"a", "b"}), -1);
    position.clear();
    EXPECT_EQ(position.restore("failed", {"a", "b"}), -1);
}

class DownloadsPositionTest : public testing::TestWithParam<look::Interface> {};
TEST_P(DownloadsPositionTest, RefreshAndRemovalKeepActionsOnTheExpectedJob)
{
    look::load("");
    look::choose(GetParam(), 0); look::keep();
    test::TestFonts fonts;
    host::Fixture backend{host::FixtureMode::normal};
    test::ScriptedTransport transport;
    test::FakeSink sink;
    Store store{transport};
    ArtCache art{transport, sink};
    Context context{fonts.type, fonts.fonts, store, art};
    DownloadsScreen screen{context};
    Feedback feedback;
    std::vector<std::string> ids;
    for (int i = 0; i < 30; ++i) ids.push_back("job-" + std::to_string(i));
    transport.reply = [&](const http::Request &request) {
        if (request.path == "/api/v1/downloads")
        {
            std::string body = "[";
            for (const auto &id : ids)
            {
                if (body.size() > 1) body += ',';
                body += "{\"id\":\"" + id + "\",\"title\":\"" + id + "\",\"status\":\"error\",\"storageId\":\"internal\",\"error\":\"Interrupted\"}";
            }
            return test::json_reply(200, body + ']');
        }
        return backend.send(request);
    };
    const auto frame = [&](bool confirm = false) {
        InputFrame input;
        input.connected = true;
        if (confirm) input.pressed = hui::action_bit(GetParam() == look::Interface::crossbar ? Action::north : Action::confirm);
        screen.update(input, 1.0f / 60.0f, feedback, true);
        Frame output; screen.draw(output, true);
    };
    const auto refresh = [&] { store.poll_once(); store.tick(); frame(); };
    refresh();
    screen.show_job("job-20"); frame();
    ids.erase(ids.begin(), ids.begin() + 5);
    refresh(); // deleting earlier rows must not silently change the selection
    frame(true); store.run_next_action(); store.tick();
    EXPECT_GT(transport.count("/api/v1/downloads/job-20/retry"), 0);
    ids.erase(std::find(ids.begin(), ids.end(), "job-20"));
    refresh();
    frame(true); store.run_next_action(); store.tick();
    EXPECT_GT(transport.count("/api/v1/downloads/job-21/retry"), 0);
    look::load("");
}
INSTANTIATE_TEST_SUITE_P(AllLayouts, DownloadsPositionTest, testing::Values(
    look::Interface::store, look::Interface::cinema, look::Interface::crossbar, look::Interface::panorama));
}
