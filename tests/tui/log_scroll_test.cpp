#include <doctest/doctest.h>

#include <cstddef>

#include "tui/log_scroll.hpp"

using tkw::tui::handle_log_key;
using tkw::tui::kLogScrollPage;
using tkw::tui::LogKey;
using tkw::tui::LogScroll;

TEST_CASE("tui: log scroll follows tail by default")
{
    LogScroll scroll;
    CHECK(scroll.follow);
    CHECK(scroll.anchor == 0);
    CHECK(scroll.ratio(100) == doctest::Approx(1.0f));
    CHECK(scroll.ratio(1) == doctest::Approx(1.0f));
    CHECK(scroll.ratio(0) == doctest::Approx(1.0f));
}

TEST_CASE("tui: log scroll moves clamps and restores follow")
{
    LogScroll scroll;
    scroll.scroll(-kLogScrollPage, 100);
    CHECK_FALSE(scroll.follow);
    CHECK(scroll.anchor == 89);
    CHECK(scroll.ratio(100) == doctest::Approx(89.0f / 99.0f));

    scroll.scroll(-kLogScrollPage, 100);
    CHECK(scroll.anchor == 79);

    scroll.scroll(-1000, 100);
    CHECK(scroll.anchor == 0);
    CHECK_FALSE(scroll.follow);
    CHECK(scroll.ratio(100) == doctest::Approx(0.0f));

    scroll.scroll(1000, 100);
    CHECK(scroll.anchor == 99);
    CHECK(scroll.follow);
    CHECK(scroll.ratio(100) == doctest::Approx(1.0f));
}

TEST_CASE("tui: log scroll scrolls up from top without following")
{
    LogScroll scroll;
    scroll.scroll(-1000, 100);  // 先把锚点压到顶部
    REQUIRE(scroll.anchor == 0);
    REQUIRE_FALSE(scroll.follow);

    scroll.scroll(kLogScrollPage, 100);
    CHECK(scroll.anchor == 10);
    CHECK_FALSE(scroll.follow);
}

TEST_CASE("tui: log scroll tolerates empty and single line logs")
{
    LogScroll scroll;
    scroll.scroll(-10, 0);
    CHECK(scroll.follow);
    CHECK(scroll.anchor == 0);
    CHECK(scroll.ratio(0) == doctest::Approx(1.0f));

    scroll.scroll(-10, 1);
    CHECK(scroll.follow);
    CHECK(scroll.anchor == 0);
    CHECK(scroll.ratio(1) == doctest::Approx(1.0f));
}

TEST_CASE("tui: log scroll anchor is absolute across new lines")
{
    LogScroll scroll;
    scroll.scroll(-10, 100);
    REQUIRE(scroll.anchor == 89);

    // 新行到达后窗口不漂移：锚点仍是绝对下标 89。
    CHECK(scroll.ratio(120) == doctest::Approx(89.0f / 119.0f));
}

TEST_CASE("tui: log scroll end and home are symmetric")
{
    LogScroll scroll;
    scroll.scroll(-10, 100);
    REQUIRE_FALSE(scroll.follow);

    scroll.to_tail(100);
    CHECK(scroll.follow);
    CHECK(scroll.anchor == 99);
    CHECK(scroll.ratio(100) == doctest::Approx(1.0f));

    scroll.to_top(100);
    CHECK_FALSE(scroll.follow);
    CHECK(scroll.anchor == 0);
    CHECK(scroll.ratio(100) == doctest::Approx(0.0f));

    scroll.to_tail(0);
    CHECK(scroll.follow);
    CHECK(scroll.anchor == 0);
    CHECK(scroll.ratio(0) == doctest::Approx(1.0f));
}

TEST_CASE("tui: log key routing consumes by key and input gate")
{
    LogScroll scroll;

    CHECK_FALSE(handle_log_key(LogKey::None, true, scroll, 100));
    CHECK(scroll.follow);

    // PgUp/PgDn 无输入门控。
    CHECK(handle_log_key(LogKey::PageUp, false, scroll, 100));
    CHECK_FALSE(scroll.follow);
    CHECK(scroll.anchor == 89);

    CHECK(handle_log_key(LogKey::PageDown, false, scroll, 100));
    CHECK(scroll.follow);
    CHECK(scroll.anchor == 99);

    // Home/End 仅命令输入为空时接管。
    CHECK_FALSE(handle_log_key(LogKey::Home, false, scroll, 100));
    CHECK(scroll.follow);
    CHECK(handle_log_key(LogKey::Home, true, scroll, 100));
    CHECK_FALSE(scroll.follow);
    CHECK(scroll.anchor == 0);

    CHECK_FALSE(handle_log_key(LogKey::End, false, scroll, 100));
    CHECK(scroll.anchor == 0);
    CHECK(handle_log_key(LogKey::End, true, scroll, 100));
    CHECK(scroll.follow);
    CHECK(scroll.anchor == 99);

    // 空日志：翻页键被消费但状态不位移。
    LogScroll empty;
    CHECK(handle_log_key(LogKey::PageUp, true, empty, 0));
    CHECK(empty.follow);
    CHECK(empty.anchor == 0);
}
