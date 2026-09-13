#include <doctest/doctest.h>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <ftxui/ftxui.hpp>

#include "decision_panel.hpp"

namespace
{
    using tkw::tui::DecisionPanel;
    using tkw::tui::DecisionPanelView;
    using tkw::tui::PanelOption;

    /** 单选面板：count 个候选，allow_pass 控制放弃是否合法。 */
    DecisionPanelView single_view(std::size_t count, bool allow_pass)
    {
        DecisionPanelView view;
        view.kind = tkw::game::ai::DecisionKind::Play;
        view.actor = "P0";
        view.title = "出牌阶段";
        view.allow_pass = allow_pass;
        for (std::size_t i = 0; i < count; ++i)
        {
            PanelOption opt;
            opt.text = "候选 " + std::to_string(i + 1);
            view.options.push_back(std::move(opt));
        }
        return view;
    }

    /** 弃牌多选面板：need_count 张、count 个候选、不可放弃。 */
    DecisionPanelView multi_view(int need_count, std::size_t count)
    {
        DecisionPanelView view;
        view.kind = tkw::game::ai::DecisionKind::Discard;
        view.actor = "P0";
        view.title = "弃牌";
        view.multi = true;
        view.toggle = true;
        view.need_count = need_count;
        view.allow_pass = false;
        for (std::size_t i = 0; i < count; ++i)
        {
            PanelOption opt;
            opt.text = "候选 " + std::to_string(i + 1);
            view.options.push_back(std::move(opt));
        }
        return view;
    }

    /**
     * 带牌面的出牌面板：第 1 项是杀（♠7，可选遮挡），第 2 项是桃（♥3）。
     * @param hidden 为真时第 1 项是隐藏占位，card_* 三项全空。
     */
    DecisionPanelView card_view(bool hidden)
    {
        DecisionPanelView view;
        view.kind = tkw::game::ai::DecisionKind::Play;
        view.actor = "P0";
        view.title = "出牌阶段";
        view.allow_pass = true;

        PanelOption first;
        first.text = hidden ? "（未知手牌）" : "杀 c1";
        if (hidden)
        {
            first.hidden = true;
        }
        else
        {
            first.card_name = "杀";
            first.card_meta = "♠7";
            first.card_text = "出牌阶段限一次，对攻击范围内的一名其他角色使用。";
        }
        view.options.push_back(std::move(first));

        PanelOption second;
        second.text = "桃 c2";
        second.card_name = "桃";
        second.card_meta = "♥3";
        second.card_text = "出牌阶段，对自己使用，回复1点体力。";
        view.options.push_back(std::move(second));
        return view;
    }

    /** 把面板渲染到固定尺寸 Screen，返回以 \r\n 分行的文本。 */
    std::string render_panel(const DecisionPanel &panel)
    {
        ftxui::Screen screen(60, 20);
        ftxui::Render(screen, panel.render());
        return screen.ToString();
    }

    /** 依次驱动 `card <序号>` 命令的按键。 */
    void type_card(DecisionPanel &panel, const std::string &rest)
    {
        panel.on_event(ftxui::Event::c);
        for (const char ch : rest)
            panel.on_event(ftxui::Event::Character(ch));
        panel.on_event(ftxui::Event::Return);
    }
}  // namespace

TEST_CASE("tui: decision panel cursor submits the highlighted single option")
{
    DecisionPanel panel;
    std::vector<std::size_t> selected;
    bool passed = true;
    int submits = 0;
    panel.set_on_submit(
        [&](std::vector<std::size_t> sel, bool pass)
        {
            selected = std::move(sel);
            passed = pass;
            ++submits;
            return true;
        });
    panel.show(single_view(3, true));
    REQUIRE(panel.visible());

    CHECK(panel.on_event(ftxui::Event::ArrowDown));
    CHECK(panel.on_event(ftxui::Event::ArrowDown));
    CHECK(panel.on_event(ftxui::Event::ArrowUp));
    CHECK(panel.on_event(ftxui::Event::Return));

    CHECK(submits == 1);
    CHECK(selected == std::vector<std::size_t>({1}));
    CHECK_FALSE(passed);
    CHECK_FALSE(panel.visible());
}

TEST_CASE("tui: decision panel p discards only when the decision allows pass")
{
    DecisionPanel panel;
    bool passed = false;
    int submits = 0;
    panel.set_on_submit(
        [&](std::vector<std::size_t>, bool pass)
        {
            ++submits;
            passed = pass;
            return true;
        });

    panel.show(single_view(2, false));
    CHECK(panel.on_event(ftxui::Event::p));
    CHECK(submits == 0);
    CHECK(panel.visible());

    panel.show(single_view(2, true));
    CHECK(panel.on_event(ftxui::Event::p));
    CHECK(submits == 1);
    CHECK(passed);
    CHECK_FALSE(panel.visible());
}

TEST_CASE("tui: decision panel multi select enforces the required count")
{
    DecisionPanel panel;
    std::vector<std::size_t> selected;
    int submits = 0;
    panel.set_on_submit(
        [&](std::vector<std::size_t> sel, bool)
        {
            selected = std::move(sel);
            ++submits;
            return true;
        });
    panel.show(multi_view(2, 3));
    REQUIRE(panel.visible());

    CHECK(panel.on_event(ftxui::Event::Character(' ')));  // 勾选第 1 项
    CHECK(panel.on_event(ftxui::Event::ArrowDown));
    CHECK(panel.on_event(ftxui::Event::Return));  // 仅 1 张：保持待决
    CHECK(submits == 0);
    CHECK(panel.visible());

    CHECK(panel.on_event(ftxui::Event::Character(' ')));  // 勾选第 2 项
    CHECK(panel.on_event(ftxui::Event::Return));
    CHECK(submits == 1);
    CHECK(selected == std::vector<std::size_t>({0, 1}));
    CHECK_FALSE(panel.visible());
}

TEST_CASE("tui: decision panel digit keys select and confirm")
{
    DecisionPanel panel;
    std::vector<std::size_t> selected;
    int submits = 0;
    panel.set_on_submit(
        [&](std::vector<std::size_t> sel, bool)
        {
            selected = std::move(sel);
            ++submits;
            return true;
        });

    // 单选：数字直选并直接确认。
    panel.show(single_view(3, false));
    CHECK(panel.on_event(ftxui::Event::Character('2')));
    CHECK(submits == 1);
    CHECK(selected == std::vector<std::size_t>({1}));
    CHECK_FALSE(panel.visible());

    // 单选：越界数字被吞掉，不提交也不落入隐藏输入。
    panel.show(single_view(2, true));
    CHECK(panel.on_event(ftxui::Event::Character('9')));
    CHECK(submits == 1);
    CHECK(panel.visible());

    // 多选：数字只切换勾选，Enter 才确认。
    panel.show(multi_view(1, 3));
    CHECK(panel.on_event(ftxui::Event::Character('3')));
    CHECK(submits == 1);
    CHECK(panel.visible());
    CHECK(panel.on_event(ftxui::Event::Return));
    CHECK(submits == 2);
    CHECK(selected == std::vector<std::size_t>({2}));
}

TEST_CASE("tui: decision panel empty options pass and cursor clamps")
{
    DecisionPanel panel;
    std::vector<std::size_t> selected;
    bool passed = false;
    int submits = 0;
    panel.set_on_submit(
        [&](std::vector<std::size_t> sel, bool pass)
        {
            selected = std::move(sel);
            passed = pass;
            ++submits;
            return true;
        });

    // 无候选且可放弃：Enter 直接放弃。
    panel.show(single_view(0, true));
    CHECK(panel.on_event(ftxui::Event::Return));
    CHECK(submits == 1);
    CHECK(passed);
    CHECK(selected.empty());
    CHECK_FALSE(panel.visible());

    // 光标在两端钳位。
    panel.show(single_view(2, false));
    CHECK(panel.on_event(ftxui::Event::ArrowUp));
    CHECK(panel.on_event(ftxui::Event::ArrowDown));
    CHECK(panel.on_event(ftxui::Event::ArrowDown));
    CHECK(panel.on_event(ftxui::Event::Return));
    CHECK(selected == std::vector<std::size_t>({1}));
}

TEST_CASE("tui: decision panel ignores unbound keys")
{
    DecisionPanel panel;
    int submits = 0;
    panel.set_on_submit(
        [&](std::vector<std::size_t>, bool)
        {
            ++submits;
            return true;
        });
    panel.show(single_view(2, true));

    CHECK_FALSE(panel.on_event(ftxui::Event::Character('z')));
    CHECK(submits == 0);
    CHECK(panel.visible());
}

TEST_CASE("tui: decision panel question mark toggles key help")
{
    DecisionPanel panel;
    panel.show(single_view(2, true));
    REQUIRE(panel.visible());

    CHECK(panel.on_event(ftxui::Event::Character('?')));
    std::string out = render_panel(panel);
    CHECK(out.find("键位") != std::string::npos);
    CHECK(out.find("card <序号>") != std::string::npos);

    CHECK(panel.on_event(ftxui::Event::Character('?')));
    out = render_panel(panel);
    CHECK(out.find("card <序号>") == std::string::npos);
}

TEST_CASE("tui: decision panel card command shows the selected card face")
{
    DecisionPanel panel;
    int submits = 0;
    panel.set_on_submit(
        [&](std::vector<std::size_t>, bool)
        {
            ++submits;
            return true;
        });
    panel.show(card_view(false));
    REQUIRE(panel.visible());

    type_card(panel, "ard 1");
    const std::string out = render_panel(panel);
    CHECK(out.find("牌面：杀") != std::string::npos);
    CHECK(out.find("♠7") != std::string::npos);
    CHECK(out.find("出牌阶段限一次") != std::string::npos);
    CHECK(submits == 0);
    CHECK(panel.visible());
}

TEST_CASE("tui: decision panel card command rejects invalid index")
{
    DecisionPanel panel;
    int submits = 0;
    panel.set_on_submit(
        [&](std::vector<std::size_t>, bool)
        {
            ++submits;
            return true;
        });
    panel.show(card_view(false));
    REQUIRE(panel.visible());

    type_card(panel, "9");
    const std::string out = render_panel(panel);
    CHECK(out.find("用法") != std::string::npos);
    CHECK(out.find("序号") != std::string::npos);
    CHECK(submits == 0);
    CHECK(panel.visible());
}

TEST_CASE("tui: decision panel digit inside card command does not submit")
{
    DecisionPanel panel;
    int submits = 0;
    panel.set_on_submit(
        [&](std::vector<std::size_t>, bool)
        {
            ++submits;
            return true;
        });
    panel.show(card_view(false));
    REQUIRE(panel.visible());

    CHECK(panel.on_event(ftxui::Event::c));
    CHECK(panel.on_event(ftxui::Event::Character('1')));
    CHECK(submits == 0);
    CHECK(panel.visible());
    CHECK(panel.on_event(ftxui::Event::Return));
    CHECK(submits == 0);
}

TEST_CASE("tui: decision panel hides opponent hand card face")
{
    DecisionPanel panel;
    panel.show(card_view(true));
    REQUIRE(panel.visible());

    type_card(panel, "1");
    const std::string out = render_panel(panel);
    CHECK(out.find("无法查看") != std::string::npos);
    CHECK(out.find("出牌阶段限一次") == std::string::npos);
    CHECK(out.find("♠7") == std::string::npos);
}
