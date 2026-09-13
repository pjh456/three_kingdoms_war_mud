/**
 * @file main.cpp
 * @brief tkw-tui 入口：静态多面板壳 + 非 TTY 守卫。
 * @note 仅验证 FTXUI 依赖接线与终端渲染，未接引擎状态；棋盘/手牌/日志/状态
 *       为固定占位。q / Esc / Ctrl-C 干净退出，退出后由 FTXUI 恢复终端。
 */

#include <iostream>
#include <string>

#include <ftxui/ftxui.hpp>
#include <pjh_platform/console.hpp>

namespace
{

    /**
     * @brief 单个带圆角边框与粗体标题的占位面板。
     * @param title 面板标题
     * @param body 面板内容
     * @return 加边框的 FTXUI 元素
     */
    auto panel(const std::string &title, ftxui::Element body) -> ftxui::Element
    {
        return ftxui::window(ftxui::text(title) | ftxui::bold, std::move(body));
    }

    /**
     * @brief 组装静态布局：棋盘 / 手牌 / 日志 / 状态四面板。
     * @return 整屏 FTXUI 元素
     * @note 数据固定，仅用于验证布局与中文/fullwidth 渲染；接入引擎后改读快照。
     */
    auto shell() -> ftxui::Element
    {
        auto board = panel("棋盘", ftxui::vbox({
            ftxui::text("P0  体力 4/4  手牌 4  距 1"),
            ftxui::text("P1  体力 4/4  手牌 4  距 1"),
        }));
        auto hand = panel("手牌", ftxui::vbox({
            ftxui::text("1. 杀    ♠7"),
            ftxui::text("2. 闪    ♥2"),
            ftxui::text("3. 桃    ♦5"),
            ftxui::text("4. 过河拆桥  ♣3"),
        }));
        auto log = panel("日志", ftxui::vbox({
            ftxui::text("[摸牌] P0 杀"),
            ftxui::text("[打出] P0 杀"),
            ftxui::text("[伤害] P0 -> P1 1"),
        }));
        auto status = panel("状态", ftxui::text(
            "第 0 回合  下一回合: P0  AI: simple  摸牌堆 108  弃牌堆 0"));

        return ftxui::vbox({
            board,
            ftxui::hbox({hand | ftxui::flex, log | ftxui::flex}),
            status,
            ftxui::text("占位壳（未接引擎）：q / Esc / Ctrl-C 退出") | ftxui::dim,
        });
    }

}  // namespace

int main(int /*argc*/, char ** /*argv*/)
{
    // 全屏渲染依赖交互式终端：管道/重定向下提前退出，避免转义序列污染输出。
    if (!pjh::platform::Console::is_tty(1))
    {
        std::cerr << "tkw-tui 需要交互式终端，当前标准输出不是 TTY；"
                     "请改用 `tkw repl`。\n";
        return 1;
    }

    auto screen = ftxui::ScreenInteractive::Fullscreen();
    auto component = ftxui::Renderer(shell) | ftxui::CatchEvent(
        [&screen](ftxui::Event event)
        {
            if (event == ftxui::Event::Character('q') ||
                event == ftxui::Event::Escape ||
                event == ftxui::Event::CtrlC)
            {
                screen.Exit();
                return true;
            }
            return false;
        });

    screen.Loop(component);
    return 0;
}
