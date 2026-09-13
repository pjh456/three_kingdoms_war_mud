/**
 * @file main.cpp
 * @brief tkw-tui 入口：argv 选项 + 真实快照四面板 + 非 TTY 守卫。
 * @note 启动时按 argv 建一局（默认 AI 对局；--human 可指定真人座位）并渲染其值
 *       快照；标准输入与标准输出任一非 TTY 即提前退出，避免全屏转义序列污染
 *       管道输出。命令栏 quit/q 或 Esc/Ctrl-C 干净退出（退出前自动存档），退出后
 *       由 FTXUI 恢复终端，自动存档结果写 stderr。
 */

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

#include <ftxui/ftxui.hpp>
#include <pjh_platform/console.hpp>

#include "app.hpp"
#include "game/core/rules.hpp"
#include "tui/command.hpp"

namespace
{
    /** argv 解析结果：error 非空表示解析失败（中文提示），否则 options 可用。 */
    struct ParsedArgs
    {
        tkw::cli::Options options;
        std::string error;
    };

    /** argv 是否请求帮助（--help/-h）；预扫描，遇 --human 等取值也不影响。 */
    bool wants_help(int argc, char **argv)
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::string_view arg = argv[i];
            if (arg == "--help" || arg == "-h")
                return true;
        }
        return false;
    }

    /**
     * @brief 解析 tkw-tui 的 argv 到启动选项。
     * @param argc/argv 原始命令行。
     * @return 解析成功时 error 为空；否则 error 为中文提示，options 不可用。
     * @note 支持 --human/--no-human/--players/--seed/--ai/--deck/--autosave/
     *       --hand/--mode；选项值域复用命令栏同一解析器，保证两入口同语义。
     *       --help/-h 由 main 在调用本函数前短路，不进入此处。
     */
    ParsedArgs parse_args(int argc, char **argv)
    {
        namespace detail = tkw::tui::detail;

        ParsedArgs parsed;
        const auto need_value = [&](int &i, std::string &out) -> bool
        {
            if (i + 1 >= argc)
                return false;
            out = argv[++i];
            return true;
        };

        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i];
            std::string value;

            if (arg == "--human")
            {
                if (!need_value(i, value))
                {
                    parsed.error = "选项 '--human' 需要一个值";
                    return parsed;
                }
                parsed.options.humans.push_back(value);
            }
            else if (arg == "--no-human")
            {
                parsed.options.humans.clear();
            }
            else if (arg == "--players")
            {
                if (!need_value(i, value))
                {
                    parsed.error = "选项 '--players' 需要一个值";
                    return parsed;
                }
                int players = 0;
                const tkw::game::RulesConfig rules;
                if (!detail::parse_i32(value, players))
                    parsed.error = "选项 '--players' 的值 '" + value +
                                   "' 无效: 期望整数";
                else if (players < rules.min_players ||
                         players > rules.max_players)
                    parsed.error = detail::player_range_error(players);
                else
                    parsed.options.players = players;
                if (!parsed.error.empty())
                    return parsed;
            }
            else if (arg == "--seed")
            {
                if (!need_value(i, value))
                {
                    parsed.error = "选项 '--seed' 需要一个值";
                    return parsed;
                }
                std::uint32_t seed = 0;
                if (!detail::parse_u32(value, seed))
                {
                    parsed.error = "选项 '--seed' 的值 '" + value +
                                   "' 无效: 期望非负整数";
                    return parsed;
                }
                parsed.options.seed = seed;
            }
            else if (arg == "--hand")
            {
                if (!need_value(i, value))
                {
                    parsed.error = "选项 '--hand' 需要一个值";
                    return parsed;
                }
                int hand = 0;
                if (!detail::parse_i32(value, hand) || hand < 0)
                {
                    parsed.error = "选项 '--hand' 的值 '" + value +
                                   "' 无效: 期望非负整数";
                    return parsed;
                }
                parsed.options.hand = hand;
            }
            else if (arg == "--ai")
            {
                if (!need_value(i, value))
                {
                    parsed.error = "选项 '--ai' 需要一个值";
                    return parsed;
                }
                tkw::cli::AiLevel ai = tkw::cli::AiLevel::Simple;
                if (!detail::ai_from(value, ai))
                {
                    parsed.error = "选项 '--ai' 的值 '" + value +
                                   "' 无效: 期望 simple 或 aggressive";
                    return parsed;
                }
                parsed.options.ai = ai;
            }
            else if (arg == "--mode")
            {
                if (!need_value(i, value))
                {
                    parsed.error = "选项 '--mode' 需要一个值";
                    return parsed;
                }
                tkw::game::GameMode mode = tkw::game::GameMode::Brawl;
                if (!detail::mode_from(value, mode))
                {
                    parsed.error = "选项 '--mode' 的值 '" + value +
                                   "' 无效: 期望 brawl 或 identity";
                    return parsed;
                }
                parsed.options.mode = mode;
            }
            else if (arg == "--deck")
            {
                if (!need_value(i, value))
                {
                    parsed.error = "选项 '--deck' 需要一个值";
                    return parsed;
                }
                parsed.options.deck = value;
            }
            else if (arg == "--autosave")
            {
                if (!need_value(i, value))
                {
                    parsed.error = "选项 '--autosave' 需要一个值";
                    return parsed;
                }
                parsed.options.autosave = value;
            }
            else if (!arg.empty() && arg[0] == '-')
            {
                parsed.error = "未知选项: '" + arg +
                               "'（支持 --human/--no-human/--players/--seed/"
                               "--hand/--ai/--mode/--deck/--autosave/--help；"
                               "查看 tkw-tui --help）";
                return parsed;
            }
            else
            {
                parsed.error = "tkw-tui 不接受位置参数: '" + arg + "'";
                return parsed;
            }
        }
        return parsed;
    }
}  // namespace

int main(int argc, char **argv)
{
    // --help/-h 是纯文本，必须在非 TTY 守卫前短路：管道下也应 rc=0 并打印命令表。
    if (wants_help(argc, argv))
    {
        for (const auto &line : tkw::tui::detail::help_lines())
            std::cout << line << "\n";
        return 0;
    }

    // 全屏渲染依赖可交互终端：stdin/stdout 任一被重定向都提前退出。
    if (!pjh::platform::Console::is_tty(0) ||
        !pjh::platform::Console::is_tty(1))
    {
        std::cerr << "tkw-tui 需要交互式终端（标准输入与标准输出均为 TTY）；"
                     "请改用 `tkw repl`。\n";
        return 1;
    }

    const ParsedArgs parsed = parse_args(argc, argv);
    if (!parsed.error.empty())
    {
        std::cerr << "参数错误: " << parsed.error << "\n";
        return 2;
    }

    auto screen = ftxui::ScreenInteractive::Fullscreen();
    tkw::tui::App app;
    app.set_base_options(parsed.options);
    app.bootstrap();
    screen.Loop(app.component(screen));
    // 面板已随全屏退出不可见：自动存档结果在此补写 stderr，失败不静默。
    if (!app.exit_message().empty())
        std::cerr << app.exit_message() << "\n";
    return 0;
}
