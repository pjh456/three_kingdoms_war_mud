/**
 * @file command.hpp
 * @brief TUI 命令栏纯解析：单行文本 → 结构化的会话命令。
 * @note 纯函数、无 FTXUI、无输出副作用：解析失败以中文文案返回 Result，由控制
 *       器写入日志面板。行内选项以调用方传入的启动选项为基准继承（未显式给出的
 *       项沿用 base），与 REPL 行内命令继承启动选项同语义。
 */
#ifndef INCLUDE_TKW_TUI_COMMAND_HPP
#define INCLUDE_TKW_TUI_COMMAND_HPP

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "cli/session.hpp"
#include "game/core/rules.hpp"
#include "util/types.hpp"

namespace tkw
{
    namespace tui
    {
        /** 命令栏支持的命令类别。 */
        enum class CommandKind : std::uint8_t
        {
            New,    /**< 开新对局（不立即推进） */
            Deal,   /**< 按位置参数开新局并直接跑到底 */
            Step,   /**< 执行一个回合 */
            Run,    /**< 跑到对局结束 */
            Status, /**< 追加会话状态摘要（不触引擎） */
            Save,   /**< 显式存档到文件 */
            Load,   /**< 从存档恢复会话 */
            Quit,   /**< 退出 TUI */
            Help,   /**< 追加命令表 */
        };

        /** 解析后的命令值：类别 + new/deal 的已解析选项 + save/load 路径。 */
        struct Command
        {
            CommandKind kind = CommandKind::Step;   /**< 命令类别 */
            tkw::cli::Options options;              /**< new/deal：base 继承 + 行内覆盖 */
            std::string file;                       /**< save/load 存档路径 */
            bool run_to_end = false;                /**< deal/run 建局或起跑后跑到底 */
        };

        /** 解析结果：Ok(Command) 或 Err(中文提示)。 */
        using CommandParseResult = tkw::Result<Command, std::string>;

        namespace detail
        {
            /** 去除首尾空白（空格与制表符）；全空白返回空串。 */
            inline std::string_view trim(std::string_view text)
            {
                const auto is_space = [](const char c)
                { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
                while (!text.empty() && is_space(text.front()))
                    text.remove_prefix(1);
                while (!text.empty() && is_space(text.back()))
                    text.remove_suffix(1);
                return text;
            }

            /** 按空白切分为 token 列表；输入假定已 trim。 */
            inline std::vector<std::string> split_ws(std::string_view text)
            {
                std::vector<std::string> tokens;
                std::size_t i = 0;
                while (i < text.size())
                {
                    while (i < text.size() &&
                           (text[i] == ' ' || text[i] == '\t'))
                        ++i;
                    if (i >= text.size())
                        break;
                    const std::size_t begin = i;
                    while (i < text.size() && text[i] != ' ' && text[i] != '\t')
                        ++i;
                    tokens.emplace_back(text.substr(begin, i - begin));
                }
                return tokens;
            }

            /** 严格解析十进制 int（整串消费、无前后缀）。 */
            inline bool parse_i32(std::string_view text, int &out)
            {
                if (text.empty())
                    return false;
                int value = 0;
                const char *begin = text.data();
                const char *end = begin + text.size();
                const auto r = std::from_chars(begin, end, value);
                if (r.ec != std::errc{} || r.ptr != end)
                    return false;
                out = value;
                return true;
            }

            /** 严格解析十进制 uint32（拒绝负号与溢出）。 */
            inline bool parse_u32(std::string_view text, std::uint32_t &out)
            {
                if (text.empty() || text.front() == '-')
                    return false;
                unsigned long long value = 0;
                const char *begin = text.data();
                const char *end = begin + text.size();
                const auto r = std::from_chars(begin, end, value);
                if (r.ec != std::errc{} || r.ptr != end ||
                    value > 0xffffffffull)
                    return false;
                out = static_cast<std::uint32_t>(value);
                return true;
            }

            /** 选项值域字符串 → 对局模式；未命中返回 false。 */
            inline bool mode_from(std::string_view name, tkw::game::GameMode &out)
            {
                if (name == "brawl")
                {
                    out = tkw::game::GameMode::Brawl;
                    return true;
                }
                if (name == "identity")
                {
                    out = tkw::game::GameMode::Identity;
                    return true;
                }
                return false;
            }

            /** 选项值域字符串 → AI 档；未命中返回 false。 */
            inline bool ai_from(std::string_view name, tkw::cli::AiLevel &out)
            {
                if (name == "simple")
                {
                    out = tkw::cli::AiLevel::Simple;
                    return true;
                }
                if (name == "aggressive")
                {
                    out = tkw::cli::AiLevel::Aggressive;
                    return true;
                }
                return false;
            }

            /** AI 档 → 命令行/存档值域字符串。 */
            inline const char *ai_level_name(tkw::cli::AiLevel ai)
            {
                return ai == tkw::cli::AiLevel::Aggressive ? "aggressive"
                                                           : "simple";
            }

            /** 玩家数越界提示：值与合法范围同 REPL 文案口径。 */
            inline std::string player_range_error(int value)
            {
                const tkw::game::RulesConfig rules;
                return "玩家数 " + std::to_string(value) + " 超出范围 [" +
                       std::to_string(rules.min_players) + ", " +
                       std::to_string(rules.max_players) + "]";
            }

            /** M1 真人决策拒绝文案（AI 局之外的真人接缝留待后续任务）。 */
            inline const char *human_rejected_error()
            {
                return "TUI M1 暂不支持真人决策，请使用 `tkw repl`";
            }

            /** 可选值缺失提示。 */
            inline std::string missing_value_error(const std::string &option)
            {
                return "选项 '" + option + "' 需要一个值";
            }

            /** 未知选项提示。 */
            inline std::string unknown_option_error(const std::string &option)
            {
                return "未知选项: '" + option + "'（help 查看用法）";
            }

            /** 解析 new 的行内选项到 cmd.options；失败返回中文文案。 */
            inline CommandParseResult parse_new(
                const std::vector<std::string> &tokens,
                const tkw::cli::Options &base)
            {
                Command cmd;
                cmd.kind = CommandKind::New;
                cmd.options = base;

                for (std::size_t i = 1; i < tokens.size(); ++i)
                {
                    const std::string &t = tokens[i];
                    const auto value_of = [&](std::string &out) -> bool
                    {
                        if (i + 1 >= tokens.size())
                            return false;
                        out = tokens[++i];
                        return true;
                    };

                    if (t == "--human")
                        return CommandParseResult::Err(human_rejected_error());

                    std::string value;
                    if (t == "--players")
                    {
                        if (!value_of(value))
                            return CommandParseResult::Err(
                                missing_value_error(t));
                        int players = 0;
                        if (!parse_i32(value, players))
                            return CommandParseResult::Err(
                                "选项 '--players' 的值 '" + value +
                                "' 无效: 期望整数");
                        if (players < tkw::game::RulesConfig{}.min_players ||
                            players > tkw::game::RulesConfig{}.max_players)
                            return CommandParseResult::Err(
                                player_range_error(players));
                        cmd.options.players = players;
                    }
                    else if (t == "--seed")
                    {
                        if (!value_of(value))
                            return CommandParseResult::Err(
                                missing_value_error(t));
                        std::uint32_t seed = 0;
                        if (!parse_u32(value, seed))
                            return CommandParseResult::Err(
                                "选项 '--seed' 的值 '" + value +
                                "' 无效: 期望非负整数");
                        cmd.options.seed = seed;
                    }
                    else if (t == "--hand")
                    {
                        if (!value_of(value))
                            return CommandParseResult::Err(
                                missing_value_error(t));
                        int hand = 0;
                        if (!parse_i32(value, hand) || hand < 0)
                            return CommandParseResult::Err(
                                "选项 '--hand' 的值 '" + value +
                                "' 无效: 期望非负整数");
                        cmd.options.hand = hand;
                    }
                    else if (t == "--mode")
                    {
                        if (!value_of(value))
                            return CommandParseResult::Err(
                                missing_value_error(t));
                        tkw::game::GameMode mode = tkw::game::GameMode::Brawl;
                        if (!mode_from(value, mode))
                            return CommandParseResult::Err(
                                "选项 '--mode' 的值 '" + value +
                                "' 无效: 期望 brawl 或 identity");
                        cmd.options.mode = mode;
                    }
                    else if (t == "--ai")
                    {
                        if (!value_of(value))
                            return CommandParseResult::Err(
                                missing_value_error(t));
                        tkw::cli::AiLevel ai = tkw::cli::AiLevel::Simple;
                        if (!ai_from(value, ai))
                            return CommandParseResult::Err(
                                "选项 '--ai' 的值 '" + value +
                                "' 无效: 期望 simple 或 aggressive");
                        cmd.options.ai = ai;
                    }
                    else if (t == "--deck")
                    {
                        if (!value_of(value))
                            return CommandParseResult::Err(
                                missing_value_error(t));
                        cmd.options.deck = value;
                    }
                    else if (t.rfind("--", 0) == 0)
                    {
                        return CommandParseResult::Err(unknown_option_error(t));
                    }
                    else
                    {
                        return CommandParseResult::Err(
                            "new 不接受位置参数: '" + t + "'");
                    }
                }
                return CommandParseResult::Ok(std::move(cmd));
            }

            /** 解析 deal 的两个位置参数 <players> <seed>。 */
            inline CommandParseResult parse_deal(
                const std::vector<std::string> &tokens,
                const tkw::cli::Options &base)
            {
                if (tokens.size() != 3)
                    return CommandParseResult::Err(
                        "deal 需要 <players> <seed> 两个参数");

                int players = 0;
                if (!parse_i32(tokens[1], players))
                    return CommandParseResult::Err(
                        "deal 的 players 值 '" + tokens[1] +
                        "' 无效: 期望整数");
                if (players < tkw::game::RulesConfig{}.min_players ||
                    players > tkw::game::RulesConfig{}.max_players)
                    return CommandParseResult::Err(player_range_error(players));

                std::uint32_t seed = 0;
                if (!parse_u32(tokens[2], seed))
                    return CommandParseResult::Err(
                        "deal 的 seed 值 '" + tokens[2] +
                        "' 无效: 期望非负整数");

                Command cmd;
                cmd.kind = CommandKind::Deal;
                cmd.options = base;
                cmd.options.players = players;
                cmd.options.seed = seed;
                cmd.run_to_end = true;
                return CommandParseResult::Ok(std::move(cmd));
            }

            /** 解析单文件参数命令（save/load）。 */
            inline CommandParseResult parse_file_command(
                CommandKind kind, const std::string &name,
                const std::vector<std::string> &tokens)
            {
                if (tokens.size() < 2)
                    return CommandParseResult::Err(
                        name + " 需要 <file> 参数");
                if (tokens.size() > 2)
                    return CommandParseResult::Err(
                        name + " 只接受一个 <file> 参数");
                Command cmd;
                cmd.kind = kind;
                cmd.file = tokens[1];
                return CommandParseResult::Ok(std::move(cmd));
            }

            /** 解析不接受参数的命令（step/run/status/quit/help）。 */
            template <typename T>
            inline tkw::Result<T, std::string> no_args(
                const std::string &name, const std::vector<std::string> &tokens,
                T value)
            {
                if (tokens.size() > 1)
                    return tkw::Result<T, std::string>::Err(
                        name + " 不接受参数");
                return tkw::Result<T, std::string>::Ok(std::move(value));
            }
        }  // namespace detail

        /**
         * @brief 解析一行命令文本。
         * @param line 用户输入的单行文本（首尾空白忽略）。
         * @param base 启动选项；new/deal 未显式给出的项沿用此基准。
         * @return Ok(Command)；Err 为面向用户的中文提示（未知命令/缺参/类型或
         *         越界/未知选项/真人拒绝），不抛异常。
         * @note 命令名与别名：run/r、status/st、quit/q、help/?。save/load 只取
         *       一个文件位置参数；new 只接受行内长选项，不接受位置参数与 --human。
         */
        inline CommandParseResult parse_command(
            std::string_view line, const tkw::cli::Options &base)
        {
            const std::string_view text = detail::trim(line);
            if (text.empty())
                return CommandParseResult::Err("空命令（help 查看用法）");

            const std::vector<std::string> tokens = detail::split_ws(text);
            const std::string &name = tokens[0];

            if (name == "new")
                return detail::parse_new(tokens, base);
            if (name == "deal")
                return detail::parse_deal(tokens, base);
            if (name == "save")
                return detail::parse_file_command(CommandKind::Save, "save",
                                                  tokens);
            if (name == "load")
                return detail::parse_file_command(CommandKind::Load, "load",
                                                  tokens);
            if (name == "step")
            {
                Command cmd;
                cmd.kind = CommandKind::Step;
                return detail::no_args<Command>("step", tokens,
                                                std::move(cmd));
            }
            if (name == "run" || name == "r")
            {
                Command cmd;
                cmd.kind = CommandKind::Run;
                cmd.run_to_end = true;
                return detail::no_args<Command>(name, tokens, std::move(cmd));
            }
            if (name == "status" || name == "st")
            {
                Command cmd;
                cmd.kind = CommandKind::Status;
                return detail::no_args<Command>(name, tokens, std::move(cmd));
            }
            if (name == "quit" || name == "q")
            {
                Command cmd;
                cmd.kind = CommandKind::Quit;
                return detail::no_args<Command>(name, tokens, std::move(cmd));
            }
            if (name == "help" || name == "?")
            {
                Command cmd;
                cmd.kind = CommandKind::Help;
                return detail::no_args<Command>(name, tokens, std::move(cmd));
            }

            // 只读与批量查询命令仅 CLI 提供：TUI 无对应命令面，明确指路而非报“未知命令”。
            if (name == "cards" || name == "rules" || name == "audit" ||
                name == "simulate")
                return CommandParseResult::Err(
                    "TUI 暂不支持 " + name + "，请退出后运行 `tkw " + name +
                    "`（REPL 内可直接用；help 查看 TUI 命令）");

            return CommandParseResult::Err("未知命令: '" + name +
                                           "'（help 查看用法）");
        }
    }  // namespace tui
}  // namespace tkw

#endif  // INCLUDE_TKW_TUI_COMMAND_HPP
