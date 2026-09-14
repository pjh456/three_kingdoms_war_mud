/**
 * @file   command.cpp
 * @brief  TUI 命令栏纯解析实现：单行文本 → 结构化的会话命令。
 * @ingroup tkw_tui
 */
#include "tui/command.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <pjh_cli/command/matcher.hpp>

namespace tkw
{
    namespace tui
    {
        namespace detail
        {
            std::string_view trim(std::string_view text)
            {
                const auto is_space = [](const char c)
                { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
                while (!text.empty() && is_space(text.front()))
                    text.remove_prefix(1);
                while (!text.empty() && is_space(text.back()))
                    text.remove_suffix(1);
                return text;
            }

            std::vector<std::string> split_ws(std::string_view text)
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

            bool parse_i32(std::string_view text, int &out)
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

            bool parse_u32(std::string_view text, std::uint32_t &out)
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

            bool mode_from(std::string_view name, tkw::game::GameMode &out)
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

            std::string player_range_error(int value)
            {
                const tkw::game::RulesConfig rules;
                return "玩家数 " + std::to_string(value) + " 超出范围 [" +
                       std::to_string(rules.min_players) + ", " +
                       std::to_string(rules.max_players) + "]";
            }

            std::string missing_value_error(const std::string &option)
            {
                return "选项 '" + option + "' 需要一个值";
            }

            std::string unknown_option_error(const std::string &option)
            {
                return "未知选项: '" + option + "'（help 查看用法）";
            }

            namespace
            {
                /**
                 * @brief 组装结构化解析错误：判别类别 + 逐字保留的中文文案。
                 * @param[in] kind   失败类别。
                 * @param[in] detail 面向用户的中文提示。
                 * @return 组装好的错误值。
                 */
                CommandParseError make_error(CommandParseError::Kind kind,
                                             std::string detail)
                {
                    return CommandParseError{kind, std::move(detail)};
                }
            }  // namespace

            tkw::Result<bool, CommandParseError> take_query_deck(
                const std::vector<std::string> &tokens, std::size_t &i,
                Command &cmd)
            {
                if (tokens[i] != "--deck")
                    return tkw::Result<bool, CommandParseError>::Ok(false);
                if (i + 1 >= tokens.size())
                    return tkw::Result<bool, CommandParseError>::Err(make_error(
                        CommandParseError::Kind::MissingValue,
                        missing_value_error("--deck")));
                cmd.options.deck = tokens[++i];
                cmd.deck_provided = true;
                return tkw::Result<bool, CommandParseError>::Ok(true);
            }

            CommandParseResult parse_new(
                const std::vector<std::string> &tokens,
                const tkw::cli::Options &base)
            {
                Command cmd;
                cmd.kind = CommandKind::New;
                cmd.options = base;
                // 行内首个 --hero 先清空启动继承值，采用 CLI 的替换语义：
                // 「启动给 P0、行内给 P1」应只留 P1，而非累积成两个座位。
                bool hero_provided = false;

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

                    std::string value;
                    if (t == "--human")
                    {
                        // 可重复累积，与命令行 --human 同语义
                        if (!value_of(value))
                            return CommandParseResult::Err(make_error(
                                CommandParseError::Kind::MissingValue,
                                missing_value_error(t)));
                        cmd.options.humans.push_back(value);
                    }
                    else if (t == "--no-human")
                    {
                        cmd.options.humans.clear();
                    }
                    else if (t == "--hero")
                    {
                        // 可重复；首次出现替换启动继承值，后续继续累积
                        if (!value_of(value))
                            return CommandParseResult::Err(make_error(
                                CommandParseError::Kind::MissingValue,
                                missing_value_error(t)));
                        if (!hero_provided)
                        {
                            cmd.options.heroes.clear();
                            hero_provided = true;
                        }
                        cmd.options.heroes.push_back(value);
                    }
                    else if (t == "--players")
                    {
                        if (!value_of(value))
                            return CommandParseResult::Err(make_error(
                                CommandParseError::Kind::MissingValue,
                                missing_value_error(t)));
                        int players = 0;
                        if (!parse_i32(value, players))
                            return CommandParseResult::Err(make_error(
                                CommandParseError::Kind::InvalidValue,
                                "选项 '--players' 的值 '" + value +
                                    "' 无效: 期望整数"));
                        if (players < tkw::game::RulesConfig{}.min_players ||
                            players > tkw::game::RulesConfig{}.max_players)
                            return CommandParseResult::Err(make_error(
                                CommandParseError::Kind::PlayerOutOfRange,
                                player_range_error(players)));
                        cmd.options.players = players;
                    }
                    else if (t == "--seed")
                    {
                        if (!value_of(value))
                            return CommandParseResult::Err(make_error(
                                CommandParseError::Kind::MissingValue,
                                missing_value_error(t)));
                        std::uint32_t seed = 0;
                        if (!parse_u32(value, seed))
                            return CommandParseResult::Err(make_error(
                                CommandParseError::Kind::InvalidValue,
                                "选项 '--seed' 的值 '" + value +
                                    "' 无效: 期望非负整数"));
                        cmd.options.seed = seed;
                    }
                    else if (t == "--hand")
                    {
                        if (!value_of(value))
                            return CommandParseResult::Err(make_error(
                                CommandParseError::Kind::MissingValue,
                                missing_value_error(t)));
                        int hand = 0;
                        if (!parse_i32(value, hand) || hand < 0)
                            return CommandParseResult::Err(make_error(
                                CommandParseError::Kind::InvalidValue,
                                "选项 '--hand' 的值 '" + value +
                                    "' 无效: 期望非负整数"));
                        cmd.options.hand = hand;
                    }
                    else if (t == "--mode")
                    {
                        if (!value_of(value))
                            return CommandParseResult::Err(make_error(
                                CommandParseError::Kind::MissingValue,
                                missing_value_error(t)));
                        tkw::game::GameMode mode = tkw::game::GameMode::Brawl;
                        if (!mode_from(value, mode))
                            return CommandParseResult::Err(make_error(
                                CommandParseError::Kind::InvalidValue,
                                "选项 '--mode' 的值 '" + value +
                                    "' 无效: 期望 brawl 或 identity"));
                        cmd.options.mode = mode;
                    }
                    else if (t == "--ai")
                    {
                        if (!value_of(value))
                            return CommandParseResult::Err(make_error(
                                CommandParseError::Kind::MissingValue,
                                missing_value_error(t)));
                        const auto ai = tkw::cli::ai_level_from(value);
                        if (ai.is_none())
                            return CommandParseResult::Err(make_error(
                                CommandParseError::Kind::InvalidValue,
                                "选项 '--ai' 的值 '" + value +
                                    "' 无效: 期望 simple 或 aggressive"));
                        cmd.options.ai = ai.unwrap();
                    }
                    else if (t == "--deck")
                    {
                        if (!value_of(value))
                            return CommandParseResult::Err(make_error(
                                CommandParseError::Kind::MissingValue,
                                missing_value_error(t)));
                        cmd.options.deck = value;
                    }
                    else if (t.rfind("--", 0) == 0)
                    {
                        return CommandParseResult::Err(make_error(
                            CommandParseError::Kind::UnknownOption,
                            unknown_option_error(t)));
                    }
                    else
                    {
                        return CommandParseResult::Err(make_error(
                            CommandParseError::Kind::UnexpectedArgument,
                            "new 不接受位置参数: '" + t + "'"));
                    }
                }
                return CommandParseResult::Ok(std::move(cmd));
            }

            CommandParseResult parse_deal(
                const std::vector<std::string> &tokens,
                const tkw::cli::Options &base)
            {
                if (tokens.size() != 3)
                    return CommandParseResult::Err(make_error(
                        CommandParseError::Kind::MissingArgument,
                        "deal 需要 <players> <seed> 两个参数"));

                int players = 0;
                if (!parse_i32(tokens[1], players))
                    return CommandParseResult::Err(make_error(
                        CommandParseError::Kind::InvalidValue,
                        "deal 的 players 值 '" + tokens[1] +
                            "' 无效: 期望整数"));
                if (players < tkw::game::RulesConfig{}.min_players ||
                    players > tkw::game::RulesConfig{}.max_players)
                    return CommandParseResult::Err(make_error(
                        CommandParseError::Kind::PlayerOutOfRange,
                        player_range_error(players)));

                std::uint32_t seed = 0;
                if (!parse_u32(tokens[2], seed))
                    return CommandParseResult::Err(make_error(
                        CommandParseError::Kind::InvalidValue,
                        "deal 的 seed 值 '" + tokens[2] +
                            "' 无效: 期望非负整数"));

                Command cmd;
                cmd.kind = CommandKind::Deal;
                cmd.options = base;
                cmd.options.players = players;
                cmd.options.seed = seed;
                cmd.run_to_end = true;
                return CommandParseResult::Ok(std::move(cmd));
            }

            CommandParseResult parse_cards(
                const std::vector<std::string> &tokens)
            {
                Command cmd;
                cmd.kind = CommandKind::Cards;
                for (std::size_t i = 1; i < tokens.size(); ++i)
                {
                    if (tokens[i] == "--text")
                    {
                        cmd.with_text = true;
                        continue;
                    }
                    auto deck = take_query_deck(tokens, i, cmd);
                    if (deck.is_err())
                        return CommandParseResult::Err(deck.unwrap_err());
                    if (deck.unwrap())
                        continue;
                    return CommandParseResult::Err(make_error(
                        CommandParseError::Kind::UnexpectedArgument,
                        "cards 只接受 --text 或 --deck <路径> 选项: '" +
                            tokens[i] + "'"));
                }
                return CommandParseResult::Ok(std::move(cmd));
            }

            CommandParseResult parse_rules(
                const std::vector<std::string> &tokens)
            {
                Command cmd;
                cmd.kind = CommandKind::Rules;
                for (std::size_t i = 1; i < tokens.size(); ++i)
                {
                    auto deck = take_query_deck(tokens, i, cmd);
                    if (deck.is_err())
                        return CommandParseResult::Err(deck.unwrap_err());
                    if (deck.unwrap())
                        continue;
                    if (tokens[i].rfind("--", 0) == 0)
                        return CommandParseResult::Err(make_error(
                            CommandParseError::Kind::UnknownOption,
                            unknown_option_error(tokens[i])));
                    if (!cmd.keyword.empty())
                        return CommandParseResult::Err(make_error(
                            CommandParseError::Kind::TooManyArguments,
                            "rules 只接受一个 <关键词> 参数"));
                    cmd.keyword = tokens[i];
                }
                return CommandParseResult::Ok(std::move(cmd));
            }

            CommandParseResult parse_audit(
                const std::vector<std::string> &tokens)
            {
                Command cmd;
                cmd.kind = CommandKind::Audit;
                for (std::size_t i = 1; i < tokens.size(); ++i)
                {
                    auto deck = take_query_deck(tokens, i, cmd);
                    if (deck.is_err())
                        return CommandParseResult::Err(deck.unwrap_err());
                    if (deck.unwrap())
                        continue;
                    return CommandParseResult::Err(make_error(
                        CommandParseError::Kind::UnexpectedArgument,
                        "audit 不接受参数: '" + tokens[i] + "'"));
                }
                return CommandParseResult::Ok(std::move(cmd));
            }

            CommandParseResult parse_decks(
                const std::vector<std::string> &tokens)
            {
                Command cmd;
                cmd.kind = CommandKind::Decks;
                for (std::size_t i = 1; i < tokens.size(); ++i)
                {
                    auto deck = take_query_deck(tokens, i, cmd);
                    if (deck.is_err())
                        return CommandParseResult::Err(deck.unwrap_err());
                    if (deck.unwrap())
                        continue;
                    return CommandParseResult::Err(make_error(
                        CommandParseError::Kind::UnexpectedArgument,
                        "decks 只接受 --deck <路径> 选项: '" + tokens[i] + "'"));
                }
                return CommandParseResult::Ok(std::move(cmd));
            }

            CommandParseResult parse_heroes(
                const std::vector<std::string> &tokens)
            {
                Command cmd;
                cmd.kind = CommandKind::Heroes;
                for (std::size_t i = 1; i < tokens.size(); ++i)
                {
                    auto deck = take_query_deck(tokens, i, cmd);
                    if (deck.is_err())
                        return CommandParseResult::Err(deck.unwrap_err());
                    if (deck.unwrap())
                        continue;
                    return CommandParseResult::Err(make_error(
                        CommandParseError::Kind::UnexpectedArgument,
                        "heroes 只接受 --deck <路径> 选项: '" + tokens[i] +
                            "'"));
                }
                return CommandParseResult::Ok(std::move(cmd));
            }

            CommandParseResult parse_simulate(
                const std::vector<std::string> &tokens,
                const tkw::cli::Options &base)
            {
                Command cmd;
                cmd.kind = CommandKind::Simulate;
                cmd.options = base;
                cmd.options.seed = 1;

                bool games_set = false;
                bool players_set = false;
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

                    auto deck = take_query_deck(tokens, i, cmd);
                    if (deck.is_err())
                        return CommandParseResult::Err(deck.unwrap_err());
                    if (deck.unwrap())
                        continue;

                    std::string value;
                    if (t == "--seed")
                    {
                        if (!value_of(value))
                            return CommandParseResult::Err(make_error(
                                CommandParseError::Kind::MissingValue,
                                missing_value_error(t)));
                        std::uint32_t seed = 0;
                        if (!parse_u32(value, seed))
                            return CommandParseResult::Err(make_error(
                                CommandParseError::Kind::InvalidValue,
                                "选项 '--seed' 的值 '" + value +
                                    "' 无效: 期望非负整数"));
                        cmd.options.seed = seed;
                    }
                    else if (t == "--hand")
                    {
                        if (!value_of(value))
                            return CommandParseResult::Err(make_error(
                                CommandParseError::Kind::MissingValue,
                                missing_value_error(t)));
                        int hand = 0;
                        if (!parse_i32(value, hand) || hand < 0)
                            return CommandParseResult::Err(make_error(
                                CommandParseError::Kind::InvalidValue,
                                "选项 '--hand' 的值 '" + value +
                                    "' 无效: 期望非负整数"));
                        cmd.options.hand = hand;
                    }
                    else if (t == "--mode")
                    {
                        if (!value_of(value))
                            return CommandParseResult::Err(make_error(
                                CommandParseError::Kind::MissingValue,
                                missing_value_error(t)));
                        tkw::game::GameMode mode = tkw::game::GameMode::Brawl;
                        if (!mode_from(value, mode))
                            return CommandParseResult::Err(make_error(
                                CommandParseError::Kind::InvalidValue,
                                "选项 '--mode' 的值 '" + value +
                                    "' 无效: 期望 brawl 或 identity"));
                        cmd.options.mode = mode;
                    }
                    else if (t == "--ai")
                    {
                        if (!value_of(value))
                            return CommandParseResult::Err(make_error(
                                CommandParseError::Kind::MissingValue,
                                missing_value_error(t)));
                        const auto ai = tkw::cli::ai_level_from(value);
                        if (ai.is_none())
                            return CommandParseResult::Err(make_error(
                                CommandParseError::Kind::InvalidValue,
                                "选项 '--ai' 的值 '" + value +
                                    "' 无效: 期望 simple 或 aggressive"));
                        cmd.options.ai = ai.unwrap();
                    }
                    else if (t.rfind("--", 0) == 0)
                    {
                        return CommandParseResult::Err(make_error(
                            CommandParseError::Kind::UnknownOption,
                            unknown_option_error(t)));
                    }
                    else if (!games_set)
                    {
                        int games = 0;
                        if (!parse_i32(t, games))
                            return CommandParseResult::Err(make_error(
                                CommandParseError::Kind::InvalidValue,
                                "simulate 的局数 '" + t +
                                    "' 无效: 期望正整数"));
                        if (games < 1)
                            return CommandParseResult::Err(make_error(
                                CommandParseError::Kind::InvalidValue,
                                "局数须为正整数"));
                        cmd.games = games;
                        games_set = true;
                    }
                    else if (!players_set)
                    {
                        int players = 0;
                        if (!parse_i32(t, players))
                            return CommandParseResult::Err(make_error(
                                CommandParseError::Kind::InvalidValue,
                                "simulate 的玩家数 '" + t +
                                    "' 无效: 期望整数"));
                        if (players < tkw::game::RulesConfig{}.min_players ||
                            players > tkw::game::RulesConfig{}.max_players)
                            return CommandParseResult::Err(make_error(
                                CommandParseError::Kind::PlayerOutOfRange,
                                player_range_error(players)));
                        cmd.options.players = players;
                        players_set = true;
                    }
                    else
                    {
                        return CommandParseResult::Err(make_error(
                            CommandParseError::Kind::UnexpectedArgument,
                            "simulate 只接受两个位置参数（局数 [玩家数]）: '" +
                                t + "'"));
                    }
                }
                if (!games_set)
                    return CommandParseResult::Err(make_error(
                        CommandParseError::Kind::MissingArgument,
                        "simulate 需要 <局数> 参数"));
                return CommandParseResult::Ok(std::move(cmd));
            }

            CommandParseResult parse_help(
                const std::string &name,
                const std::vector<std::string> &tokens)
            {
                if (tokens.size() > 2)
                    return CommandParseResult::Err(make_error(
                        CommandParseError::Kind::TooManyArguments,
                        name + " 只接受一个 <关键词> 参数"));
                Command cmd;
                cmd.kind = CommandKind::Help;
                if (tokens.size() == 2)
                    cmd.keyword = tokens[1];
                return CommandParseResult::Ok(std::move(cmd));
            }

            CommandParseResult parse_file_command(
                CommandKind kind, const std::string &name,
                const std::vector<std::string> &tokens)
            {
                if (tokens.size() < 2)
                    return CommandParseResult::Err(make_error(
                        CommandParseError::Kind::MissingArgument,
                        name + " 需要 <file> 参数"));
                if (tokens.size() > 2)
                    return CommandParseResult::Err(make_error(
                        CommandParseError::Kind::TooManyArguments,
                        name + " 只接受一个 <file> 参数"));
                Command cmd;
                cmd.kind = kind;
                cmd.file = tokens[1];
                return CommandParseResult::Ok(std::move(cmd));
            }

            const std::vector<std::string> &command_names()
            {
                static const std::vector<std::string> names = {
                    "new",    "deal", "step", "run",    "r",     "status",
                    "st",     "save", "w",    "load",   "l",     "quit",
                    "q",      "help", "?",    "cards",  "rules", "audit",
                    "decks",  "heroes", "simulate"};
                return names;
            }

            std::vector<std::string> suggest_commands(std::string_view token)
            {
                constexpr int kMaxDistance = 2;
                constexpr std::size_t kMaxSuggestions = 3;

                std::vector<std::pair<int, std::string>> scored;
                for (const auto &name : command_names())
                {
                    const int length_gap = static_cast<int>(token.size()) -
                                           static_cast<int>(name.size());
                    if (length_gap > kMaxDistance || length_gap < -kMaxDistance)
                        continue;
                    const int distance = pjh::cli::edit_distance(token, name);
                    if (distance <= kMaxDistance)
                        scored.emplace_back(distance, name);
                }

                std::stable_sort(
                    scored.begin(), scored.end(),
                    [](const auto &a, const auto &b)
                    { return a.first < b.first; });

                std::vector<std::string> out;
                for (std::size_t i = 0;
                     i < scored.size() && i < kMaxSuggestions; ++i)
                    out.push_back(scored[i].second);
                return out;
            }

            bool contains_ci(std::string_view haystack, std::string_view needle)
            {
                if (needle.empty())
                    return true;
                const auto lower = [](char c) -> char
                {
                    return (c >= 'A' && c <= 'Z')
                               ? static_cast<char>(c - 'A' + 'a')
                               : c;
                };
                for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i)
                {
                    std::size_t j = 0;
                    while (j < needle.size() &&
                           lower(haystack[i + j]) == lower(needle[j]))
                        ++j;
                    if (j == needle.size())
                        return true;
                }
                return false;
            }

            std::vector<std::string> help_lines()
            {
                const tkw::game::RulesConfig rules;
                return {
                    "TUI 命令与用法（help/? 或启动 --help/-h）：",
                    "  new [--players N] [--seed S] [--hand N] [--mode brawl|identity] "
                    "[--ai simple|aggressive] [--deck P] [--human <座位>] [--no-human] "
                    "[--hero 座位=武将]",
                    "  deal <players> <seed>；step；run/r；status/st；save/w <file>；"
                    "load/l <file>；quit/q；help/?",
                    "  simulate <局数> [玩家数] [--seed S] [--ai simple|aggressive] "
                    "[--hand N] [--mode brawl|identity] [--deck 路径]"
                    "（全 AI 批量，基种子缺省 1，结果写入本面板；q 可取消）",
                    "  cards [--text] [--deck 路径]；rules [关键词] [--deck 路径]；"
                    "audit [--deck 路径]；decks [--deck 路径]；heroes [--deck 路径]"
                    "（只读牌表/武将查询，结果写入本面板）",
                    "  牌表优先序（只读查询与 simulate）: 行内 --deck > 活动会话 > 启动 --deck",
                    "  极大批量（数百局以上）建议退出后用 tkw simulate 跑（脚本化、无 UI 线程）",
                    "  默认: --players " +
                        std::to_string(tkw::cli::Options{}.players) + "、--seed " +
                        std::to_string(tkw::cli::Options{}.seed) + "、--hand " +
                        std::to_string(rules.initial_hand) + "、--mode brawl、--ai simple",
                    "  启动选项: --human/--no-human/--hero/--players/--seed/--hand/"
                    "--ai/--mode/--deck/--autosave",
                    "  键位: Esc/Ctrl-C 退出；PgUp/PgDn 翻日志；命令栏为空时 "
                    "End 回最新、Home 到最早",
                    "  事件日志恒开；TUI 不提供 --verbose/--no-verbose",
                };
            }

            std::vector<std::string> query_help_lines(std::string_view keyword)
            {
                const std::vector<std::string> all = help_lines();
                const std::string_view key = trim(keyword);
                if (key.empty())
                    return all;

                std::vector<std::string> filtered;
                for (const auto &line : all)
                    if (contains_ci(line, key))
                        filtered.push_back(line);
                if (filtered.empty())
                    return {"没有匹配的帮助条目: '" + std::string(key) +
                            "'（help 查看全部）"};
                return filtered;
            }
        }  // namespace detail

        CommandParseResult parse_command(std::string_view line,
                                         const tkw::cli::Options &base)
        {
            const std::string_view text = detail::trim(line);
            if (text.empty())
                return CommandParseResult::Err(
                    detail::make_error(CommandParseError::Kind::EmptyCommand,
                                       "空命令（help 查看用法）"));

            const std::vector<std::string> tokens = detail::split_ws(text);
            const std::string &name = tokens[0];

            if (name == "new")
                return detail::parse_new(tokens, base);
            if (name == "deal")
                return detail::parse_deal(tokens, base);
            if (name == "save" || name == "w")
                return detail::parse_file_command(CommandKind::Save, "save",
                                                  tokens);
            if (name == "load" || name == "l")
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
                return detail::parse_help(name, tokens);
            if (name == "cards")
                return detail::parse_cards(tokens);
            if (name == "rules")
                return detail::parse_rules(tokens);
            if (name == "audit")
                return detail::parse_audit(tokens);
            if (name == "decks")
                return detail::parse_decks(tokens);
            if (name == "heroes")
                return detail::parse_heroes(tokens);
            if (name == "simulate")
                return detail::parse_simulate(tokens, base);

            // 邻近拼写给 did-you-mean 候选；无候选时保持原有的泛化提示文案。
            const std::vector<std::string> suggestions =
                detail::suggest_commands(name);
            if (suggestions.empty())
                return CommandParseResult::Err(detail::make_error(
                    CommandParseError::Kind::UnknownCommand,
                    "未知命令: '" + name + "'（help 查看用法）"));

            std::string hint = "是否想输入: ";
            for (std::size_t i = 0; i < suggestions.size(); ++i)
            {
                if (i > 0)
                    hint += " / ";
                hint += suggestions[i];
            }
            hint += "？ / ";
            return CommandParseResult::Err(detail::make_error(
                CommandParseError::Kind::UnknownCommand,
                "未知命令: '" + name + "'（" + hint + "help 查看用法）"));
        }
    }  // namespace tui
}  // namespace tkw
