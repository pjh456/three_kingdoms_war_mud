#include <doctest/doctest.h>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <ftxui/ftxui.hpp>

#include "render.hpp"

namespace
{
    using tkw::tui::CardRow;
    using tkw::tui::PlayerRow;
    using tkw::tui::UiSnapshot;
    using tkw::tui::detail::LayoutMode;
    using tkw::tui::detail::plan_layout;

    /** 渲染整屏到固定尺寸 Screen，返回以 \r\n 分行的文本。 */
    std::string render_text(UiSnapshot &snap, std::vector<std::string> &lines,
                            std::string &notice, ftxui::Element bottom, int cols,
                            int rows, float ratio = 1.0f,
                            bool expanded = false)
    {
        const tkw::tui::detail::ShellSpec spec{
            snap, lines, notice, std::move(bottom), {cols, rows}, ratio, expanded};
        ftxui::Screen screen(cols, rows);
        ftxui::Render(screen, tkw::tui::detail::render_shell(spec));
        return screen.ToString();
    }

    /** needle 首次出现的行号；未出现返回 -1。 */
    int line_of(const std::string &out, const std::string &needle)
    {
        int line = 0;
        std::size_t pos = 0;
        while (true)
        {
            const std::size_t nl = out.find("\r\n", pos);
            const std::size_t end = nl == std::string::npos ? out.size() : nl;
            if (out.substr(pos, end - pos).find(needle) != std::string::npos)
                return line;
            if (nl == std::string::npos)
                return -1;
            pos = nl + 2;
            ++line;
        }
    }

    /** 单玩家、未开局快照：棋盘/状态均有稳定可断言的文本。 */
    UiSnapshot base_snapshot()
    {
        UiSnapshot snap;
        PlayerRow row;
        row.id = "P0";
        row.seat = 0;
        row.hp = 4;
        row.max_hp = 4;
        row.hand.count = 2;
        row.hand.revealed = false;
        row.equip.count = 1;
        row.equip.revealed = true;
        CardRow equip;
        equip.instance_id = "c1";
        equip.def_id = "chitu";
        equip.display_name = "赤兔";
        row.equip.cards.push_back(equip);
        row.judge.count = 0;
        row.judge.revealed = true;
        row.distance = 1;
        snap.players.push_back(std::move(row));
        snap.viewer = "P0";
        return snap;
    }
}  // namespace

TEST_CASE("tui render: full layout keeps board hand status and command row")
{
    UiSnapshot snap = base_snapshot();
    std::vector<std::string> lines{"L1"};
    std::string notice = "提示";
    const std::string out =
        render_text(snap, lines, notice, ftxui::text("__INPUT__"), 100, 30);

    CHECK(out.find("__INPUT__") != std::string::npos);
    CHECK(out.find("棋盘") != std::string::npos);
    CHECK(out.find("手牌") != std::string::npos);
    CHECK(out.find("状态") != std::string::npos);
    CHECK(out.find("终端过小") == std::string::npos);
}

TEST_CASE("tui render: log growth does not shift board or command row")
{
    UiSnapshot snap = base_snapshot();
    std::vector<std::string> one{"L1"};
    std::vector<std::string> many;
    for (int i = 1; i <= 40; ++i)
        many.push_back("L" + std::to_string(i));
    std::string notice = "提示";

    const std::string small =
        render_text(snap, one, notice, ftxui::text("__INPUT__"), 100, 30);
    const std::string big =
        render_text(snap, many, notice, ftxui::text("__INPUT__"), 100, 30);

    REQUIRE(line_of(small, "__INPUT__") >= 0);
    CHECK(line_of(small, "__INPUT__") == line_of(big, "__INPUT__"));
    CHECK(line_of(small, "棋盘") == line_of(big, "棋盘"));
}

TEST_CASE("tui render: compact terminal keeps command row and warns")
{
    UiSnapshot snap = base_snapshot();
    std::vector<std::string> lines{"L1"};
    std::string notice = "提示";
    const std::string out =
        render_text(snap, lines, notice, ftxui::text("__INPUT__"), 80, 20);

    CHECK(out.find("__INPUT__") != std::string::npos);
    CHECK(out.find("终端过小") != std::string::npos);
    CHECK(out.find("体力") == std::string::npos);  // 棋盘已隐藏
}

TEST_CASE("tui render: minimal terminal keeps command row and hides status")
{
    UiSnapshot snap = base_snapshot();
    std::vector<std::string> lines{"L1"};
    std::string notice = "提示";
    const std::string out =
        render_text(snap, lines, notice, ftxui::text("__INPUT__"), 80, 10);

    CHECK(out.find("__INPUT__") != std::string::npos);
    CHECK(out.find("终端过小") != std::string::npos);
    CHECK(out.find("没有进行中") == std::string::npos);  // 状态面板已隐藏
}

TEST_CASE("tui render: log viewport clamps at top and tail")
{
    UiSnapshot snap = base_snapshot();
    std::vector<std::string> lines;
    for (int i = 1; i <= 100; ++i)
    {
        const std::string digits = std::to_string(i);
        lines.push_back("L" + std::string(3 - digits.size(), '0') + digits);
    }
    std::string notice = "提示";

    const std::string tail =
        render_text(snap, lines, notice, ftxui::text("__INPUT__"), 100, 30, 1.0f);
    const std::string top =
        render_text(snap, lines, notice, ftxui::text("__INPUT__"), 100, 30, 0.0f);

    CHECK(tail.find("L100") != std::string::npos);
    CHECK(tail.find("L001") == std::string::npos);
    CHECK(top.find("L001") != std::string::npos);
    CHECK(top.find("L100") == std::string::npos);

    // 内容溢出可见区时右侧出现滚动条（竖条三态之一）。
    CHECK((tail.find("┃") != std::string::npos ||
           tail.find("╻") != std::string::npos ||
           tail.find("╹") != std::string::npos));
}

TEST_CASE("tui render: expanded bottom forces compact layout")
{
    UiSnapshot snap = base_snapshot();
    std::vector<std::string> lines{"L1"};
    std::string notice = "提示";
    const std::string out = render_text(snap, lines, notice,
                                        ftxui::text("__INPUT__"), 100, 30, 1.0f,
                                        true);

    CHECK(out.find("__INPUT__") != std::string::npos);
    CHECK(out.find("终端过小") != std::string::npos);
    CHECK(out.find("体力") == std::string::npos);  // 棋盘为决策面板让位
}

TEST_CASE("tui render: layout tiers follow terminal rows")
{
    CHECK(plan_layout({80, 24}, false) == LayoutMode::Full);
    CHECK(plan_layout({80, 24}, true) == LayoutMode::Compact);
    CHECK(plan_layout({79, 24}, false) == LayoutMode::Compact);
    CHECK(plan_layout({80, 20}, false) == LayoutMode::Compact);
    CHECK(plan_layout({80, 10}, false) == LayoutMode::Minimal);
}

TEST_CASE("tui render: single panels keep board fields and viewer hand")
{
    UiSnapshot snap = base_snapshot();
    snap.players[0].hand.revealed = true;
    CardRow card;
    card.instance_id = "c2";
    card.def_id = "sha";
    card.display_name = "杀";
    card.suit = tkw::card::Suit::Spade;
    card.number = 7;
    snap.players[0].hand.cards.push_back(std::move(card));

    const auto render_one = [](ftxui::Element element)
    {
        ftxui::Screen screen(60, 10);
        ftxui::Render(screen, element);
        return screen.ToString();
    };

    const std::string board =
        render_one(tkw::tui::detail::render_board(snap));
    CHECK(board.find("体力 4/4") != std::string::npos);
    CHECK(board.find("手牌 2") != std::string::npos);
    CHECK(board.find("装备 赤兔") != std::string::npos);

    const std::string hand = render_one(tkw::tui::detail::render_hand(snap));
    CHECK(hand.find("1. 杀") != std::string::npos);
}

TEST_CASE("tui render: status panel shows alive count")
{
    UiSnapshot snap = base_snapshot();
    snap.active = true;
    snap.alive = 2;
    snap.turns = 3;
    snap.current = "P1";

    const auto render_one = [](ftxui::Element element)
    {
        ftxui::Screen screen(60, 10);
        ftxui::Render(screen, element);
        return screen.ToString();
    };

    const std::string status =
        render_one(tkw::tui::detail::render_status(snap));
    CHECK(status.find("存活: 2") != std::string::npos);

    snap.over = true;
    snap.winner_label = "P0";
    const std::string over =
        render_one(tkw::tui::detail::render_status(snap));
    CHECK(over.find("存活: 2") != std::string::npos);
}
