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
