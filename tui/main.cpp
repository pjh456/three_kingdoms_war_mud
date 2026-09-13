/**
 * @file main.cpp
 * @brief tkw-tui 入口：真实快照四面板 + 非 TTY 守卫。
 * @note 启动时建一局默认 AI 对局并渲染其值快照；标准输入与标准输出任一非 TTY
 *       即提前退出，避免全屏转义序列污染管道输出。命令栏 quit/q 或 Esc/Ctrl-C
 *       干净退出（退出前自动存档），退出后由 FTXUI 恢复终端，自动存档结果写
 *       stderr。
 */

#include <iostream>

#include <ftxui/ftxui.hpp>
#include <pjh_platform/console.hpp>

#include "app.hpp"

int main(int /*argc*/, char ** /*argv*/)
{
    // 全屏渲染依赖可交互终端：stdin/stdout 任一被重定向都提前退出。
    if (!pjh::platform::Console::is_tty(0) ||
        !pjh::platform::Console::is_tty(1))
    {
        std::cerr << "tkw-tui 需要交互式终端（标准输入与标准输出均为 TTY）；"
                     "请改用 `tkw repl`。\n";
        return 1;
    }

    auto screen = ftxui::ScreenInteractive::Fullscreen();
    tkw::tui::App app;
    app.bootstrap();
    screen.Loop(app.component(screen));
    // 面板已随全屏退出不可见：自动存档结果在此补写 stderr，失败不静默。
    if (!app.exit_message().empty())
        std::cerr << app.exit_message() << "\n";
    return 0;
}
