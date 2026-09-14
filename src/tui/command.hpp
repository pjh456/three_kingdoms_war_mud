/**
 * @file   command.hpp
 * @brief  TUI 命令栏纯解析：单行文本 → 结构化的会话命令。
 * @details 纯函数、无 FTXUI、无输出副作用：解析失败以中文文案返回 `Result`，
 *          由控制器写入日志面板。行内选项以调用方传入的启动选项为基准继承
 *          （未显式给出的项沿用 `base`），与 REPL 行内命令继承启动选项同语义。
 * @ingroup tkw_tui
 */
#ifndef INCLUDE_TKW_TUI_COMMAND_HPP
#define INCLUDE_TKW_TUI_COMMAND_HPP

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <pjh_cli/command/matcher.hpp>

#include "cli/session.hpp"
#include "game/core/rules.hpp"
#include "util/types.hpp"

namespace tkw
{
    namespace tui
    {
        /** @brief 命令栏支持的命令类别。 */
        enum class CommandKind : std::uint8_t
        {
            New,    /**< 开新对局（不立即推进）。 */
            Deal,   /**< 按位置参数开新局并直接跑到底。 */
            Step,   /**< 执行一个回合。 */
            Run,    /**< 跑到对局结束。 */
            Status, /**< 追加会话状态摘要（不触引擎）。 */
            Save,   /**< 显式存档到文件。 */
            Load,   /**< 从存档恢复会话。 */
            Quit,   /**< 退出 TUI。 */
            Help,   /**< 追加命令表。 */
            Cards,  /**< 列出牌表（结果写日志面板）。 */
            Rules,  /**< 查询卡牌说明（结果写日志面板）。 */
            Audit,  /**< 审计牌堆（结果写日志面板）。 */
            Decks,  /**< 列出可用牌表（结果写日志面板）。 */
            Heroes, /**< 列出可用武将（结果写日志面板）。 */
            Simulate, /**< 批量模拟全 AI 对局（结果写日志面板，worker 执行）。 */
        };

        /** @brief 解析后的命令值：类别 + new/deal 选项 + save/load 路径 + 查询参数。 */
        struct Command
        {
            CommandKind kind = CommandKind::Step;   /**< 命令类别。 */
            tkw::cli::Options options;              /**< new/deal：base 继承 + 行内覆盖。 */
            std::string file;                       /**< save/load 存档路径。 */
            bool run_to_end = false;                /**< deal/run 建局或起跑后跑到底。 */
            std::string keyword;                    /**< rules：过滤关键词；空 = 全部。 */
            bool with_text = false;                 /**< cards：附 CardDef.text 效果文案。 */
            bool deck_provided = false;             /**< 行内 --deck 是否显式给出（查询类）。 */
            int games = 0;                          /**< simulate：局数（≥1）。 */
        };

        /** @brief 解析结果：`Ok(Command)` 或 `Err(中文提示)`。 */
        using CommandParseResult = tkw::Result<Command, std::string>;

        namespace detail
        {
            /**
             * @brief  去除首尾空白（空格与制表符）；全空白返回空串。
             * @param[in] text 待处理文本。
             * @return 去掉首尾空白后的子串视图，指向 `text` 底层缓冲。
             */
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

            /**
             * @brief  按空白切分为 token 列表；输入假定已 trim。
             * @param[in] text 输入文本。
             * @return 按空白分隔的非空 token 列表。
             */
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

            /**
             * @brief  严格解析十进制 `int`（整串消费、无前后缀）。
             * @param[in]  text 待解析文本。
             * @param[out] out  解析成功时写入结果。
             * @return 解析成功返回 true。
             * @retval true  整串均为合法十进制且可放入 `int`。
             * @retval false 空串、含非法字符或溢出；`out` 不被修改。
             */
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

            /**
             * @brief  严格解析十进制 `uint32`（拒绝负号与溢出）。
             * @param[in]  text 待解析文本。
             * @param[out] out  解析成功时写入结果。
             * @return 解析成功返回 true。
             * @retval true  整串均为合法非负十进制且不超过 `uint32` 上限。
             * @retval false 空串、负号、含非法字符或溢出；`out` 不被修改。
             */
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

            /**
             * @brief  选项值域字符串 → 对局模式。
             * @param[in]  name 选项值（`brawl`/`identity`）。
             * @param[out] out  命中时写入模式。
             * @return 命中返回 true；未命中返回 false 且不写 `out`。
             */
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

            /**
             * @brief  选项值域字符串 → AI 档。
             * @param[in]  name 选项值（`simple`/`aggressive`）。
             * @param[out] out  命中时写入 AI 档。
             * @return 命中返回 true；未命中返回 false 且不写 `out`。
             */
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

            /**
             * @brief  AI 档 → 命令行/存档值域字符串。
             * @param[in] ai AI 档。
             * @return 稳定字面量（`"aggressive"`/`"simple"`）。
             */
            inline const char *ai_level_name(tkw::cli::AiLevel ai)
            {
                return ai == tkw::cli::AiLevel::Aggressive ? "aggressive"
                                                           : "simple";
            }

            /**
             * @brief  玩家数越界提示：值与合法范围同 REPL 文案口径。
             * @param[in] value 越界的玩家数。
             * @return 中文提示文本。
             */
            inline std::string player_range_error(int value)
            {
                const tkw::game::RulesConfig rules;
                return "玩家数 " + std::to_string(value) + " 超出范围 [" +
                       std::to_string(rules.min_players) + ", " +
                       std::to_string(rules.max_players) + "]";
            }

            /**
             * @brief  可选值缺失提示。
             * @param[in] option 缺少取值的选项名。
             * @return 中文提示文本。
             */
            inline std::string missing_value_error(const std::string &option)
            {
                return "选项 '" + option + "' 需要一个值";
            }

            /**
             * @brief  未知选项提示。
             * @param[in] option 未知选项名。
             * @return 中文提示文本。
             */
            inline std::string unknown_option_error(const std::string &option)
            {
                return "未知选项: '" + option + "'（help 查看用法）";
            }

            /**
             * @brief  只读查询命令识别并消费行内 `--deck`。
             * @param[in]     tokens 全 token 列表。
             * @param[in,out] i      token 下标引用；命中时前移到值 token 并消费。
             * @param[in,out] cmd    目标命令；命中时写入 `options.deck` 并置
             *                       `deck_provided`。
             * @return 非 `--deck` token 返回 `Ok(false)`；命中且取值成功返回
             *         `Ok(true)`；命中但缺值返回 `Err`（「需要一个值」，与
             *         new/CLI 同文案）。
             * @retval Ok(false) 当前 token 不是 `--deck`。
             * @retval Ok(true)  已消费 `--deck` 及其值。
             * @retval Err       `--deck` 后缺值；`i` 与 `cmd` 不被修改。
             */
            inline tkw::Result<bool, std::string> take_query_deck(
                const std::vector<std::string> &tokens, std::size_t &i,
                Command &cmd)
            {
                if (tokens[i] != "--deck")
                    return tkw::Result<bool, std::string>::Ok(false);
                if (i + 1 >= tokens.size())
                    return tkw::Result<bool, std::string>::Err(
                        missing_value_error("--deck"));
                cmd.options.deck = tokens[++i];
                cmd.deck_provided = true;
                return tkw::Result<bool, std::string>::Ok(true);
            }

            /**
             * @brief  解析 new 的行内选项到 `cmd.options`；失败返回中文文案。
             * @param[in] tokens 全 token 列表（`tokens[0] == "new"`）。
             * @param[in] base   启动选项；未显式覆盖的项沿用此基准。
             * @return 成功返回 `Ok(Command)`；未知选项/缺值/类型或越界返回 `Err`。
             */
            inline CommandParseResult parse_new(
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
                            return CommandParseResult::Err(
                                missing_value_error(t));
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
                            return CommandParseResult::Err(
                                missing_value_error(t));
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

            /**
             * @brief  解析 deal 的两个位置参数 `<players>` 与 `<seed>`。
             * @param[in] tokens 全 token 列表（`tokens[0] == "deal"`）。
             * @param[in] base   启动选项；两位置参数覆盖 players/seed。
             * @return 成功返回 `Ok(Command{Deal, run_to_end=true})`；参数个数、
             *         类型或范围非法返回中文 `Err`。
             */
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

            /**
             * @brief  解析 cards 的可选 `--text` 与行内 `--deck`；其余 token 报错。
             * @param[in] tokens 全 token 列表（`tokens[0] == "cards"`）。
             * @return 成功返回 `Ok(Command{Cards})`；未知 token/缺值返回中文 `Err`。
             */
            inline CommandParseResult parse_cards(
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
                    return CommandParseResult::Err(
                        "cards 只接受 --text 或 --deck <路径> 选项: '" +
                        tokens[i] + "'");
                }
                return CommandParseResult::Ok(std::move(cmd));
            }

            /**
             * @brief  解析 rules 的可选关键词（至多一个）与行内 `--deck`。
             * @param[in] tokens 全 token 列表（`tokens[0] == "rules"`）。
             * @return 成功返回 `Ok(Command{Rules, keyword})`；重复关键词/未知选项/
             *         缺值返回中文 `Err`。
             * @note   选项与关键词可任意顺序混写。
             */
            inline CommandParseResult parse_rules(
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
                        return CommandParseResult::Err(
                            unknown_option_error(tokens[i]));
                    if (!cmd.keyword.empty())
                        return CommandParseResult::Err(
                            "rules 只接受一个 <关键词> 参数");
                    cmd.keyword = tokens[i];
                }
                return CommandParseResult::Ok(std::move(cmd));
            }

            /**
             * @brief  解析 audit 的可选行内 `--deck`；其余 token 报错。
             * @param[in] tokens 全 token 列表（`tokens[0] == "audit"`）。
             * @return 成功返回 `Ok(Command{Audit})`；未知 token/缺值返回中文 `Err`。
             */
            inline CommandParseResult parse_audit(
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
                    return CommandParseResult::Err(
                        "audit 不接受参数: '" + tokens[i] + "'");
                }
                return CommandParseResult::Ok(std::move(cmd));
            }

            /**
             * @brief  解析 decks 的可选行内 `--deck`（扫描根目录）；其余 token 报错。
             * @param[in] tokens 全 token 列表（`tokens[0] == "decks"`）。
             * @return 成功返回 `Ok(Command{Decks})`；未知 token/缺值返回中文 `Err`。
             * @note   扫描根缺省与 cards/rules/audit 同源（行内 `--deck` > 活动
             *         会话 > 启动），故 decks 无参数即列出当前牌表所在目录的内置
             *         牌表。
             */
            inline CommandParseResult parse_decks(
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
                    return CommandParseResult::Err(
                        "decks 只接受 --deck <路径> 选项: '" + tokens[i] + "'");
                }
                return CommandParseResult::Ok(std::move(cmd));
            }

            /**
             * @brief  解析 heroes 的可选行内 `--deck`（武将数据根目录）；其余
             *         token 报错。
             * @param[in] tokens 全 token 列表（`tokens[0] == "heroes"`）。
             * @return 成功返回 `Ok(Command{Heroes})`；未知 token/缺值返回中文 `Err`。
             * @note   武将数据与牌表同根，缺省目录与 cards/rules/audit/decks 同源
             *         （行内 `--deck` > 活动会话 > 启动），故 heroes 无参数即列出
             *         当前牌表目录的 `heroes.json`。
             */
            inline CommandParseResult parse_heroes(
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
                    return CommandParseResult::Err(
                        "heroes 只接受 --deck <路径> 选项: '" + tokens[i] + "'");
                }
                return CommandParseResult::Ok(std::move(cmd));
            }

            /**
             * @brief  解析 `simulate <局数> [玩家数]` 与行内
             *         `--seed`/`--ai`/`--mode`/`--hand`/`--deck`。
             * @param[in] tokens 全 token 列表（`tokens[0] == "simulate"`）。
             * @param[in] base   启动选项；未显式给出的项沿用 `base`，但 seed 基值
             *                   固定为 1。
             * @return `Ok(Command{Simulate, games, options})`；缺局数/局数非正/
             *         玩家数越界或非法/多余位置参数/未知选项返回中文 `Err`。
             * @note   基种子缺省 1（局种子 1..N）对齐 CLI simulate，与
             *         `Options.seed` 缺省 42 不同；行内 `--deck` 置
             *         `deck_provided`，控制器据此覆盖活动会话/启动牌表。
             */
            inline CommandParseResult parse_simulate(
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
                    else if (t.rfind("--", 0) == 0)
                    {
                        return CommandParseResult::Err(unknown_option_error(t));
                    }
                    else if (!games_set)
                    {
                        int games = 0;
                        if (!parse_i32(t, games))
                            return CommandParseResult::Err(
                                "simulate 的局数 '" + t +
                                "' 无效: 期望正整数");
                        if (games < 1)
                            return CommandParseResult::Err("局数须为正整数");
                        cmd.games = games;
                        games_set = true;
                    }
                    else if (!players_set)
                    {
                        int players = 0;
                        if (!parse_i32(t, players))
                            return CommandParseResult::Err(
                                "simulate 的玩家数 '" + t +
                                "' 无效: 期望整数");
                        if (players < tkw::game::RulesConfig{}.min_players ||
                            players > tkw::game::RulesConfig{}.max_players)
                            return CommandParseResult::Err(
                                player_range_error(players));
                        cmd.options.players = players;
                        players_set = true;
                    }
                    else
                    {
                        return CommandParseResult::Err(
                            "simulate 只接受两个位置参数（局数 [玩家数]）: '" +
                            t + "'");
                    }
                }
                if (!games_set)
                    return CommandParseResult::Err("simulate 需要 <局数> 参数");
                return CommandParseResult::Ok(std::move(cmd));
            }

            /**
             * @brief  解析 `help`/`?` 的可选关键词（至多一个）。
             * @param[in] name   实际输入的命令名（`help` 或 `?`），用于参数错误文案。
             * @param[in] tokens 全 token 列表（`tokens[0]` 为命令名）。
             * @return `Ok(Command{Help, keyword})`；多于一个参数返回 `Err`。
             * @note   关键词经 `Command::keyword` 承载，由 `query_help_lines` 过滤；
             *         空 = 全量表。
             */
            inline CommandParseResult parse_help(
                const std::string &name,
                const std::vector<std::string> &tokens)
            {
                if (tokens.size() > 2)
                    return CommandParseResult::Err(
                        name + " 只接受一个 <关键词> 参数");
                Command cmd;
                cmd.kind = CommandKind::Help;
                if (tokens.size() == 2)
                    cmd.keyword = tokens[1];
                return CommandParseResult::Ok(std::move(cmd));
            }

            /**
             * @brief  解析单文件参数命令（save/load）。
             * @param[in] kind   目标命令类别。
             * @param[in] name   命令名，用于错误文案。
             * @param[in] tokens 全 token 列表（`tokens[0]` 为命令名）。
             * @return `Ok(Command{kind, file})`；缺文件或多于一个文件参数返回 `Err`。
             */
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

            /**
             * @brief  解析不接受参数的命令（step/run/status/quit/help）。
             * @tparam T 成功时的结果值类型。
             * @param[in] name   命令名，用于错误文案。
             * @param[in] tokens 全 token 列表（`tokens[0]` 为命令名）。
             * @param[in] value  无多余参数时返回的值。
             * @return `Ok(value)`；存在多余参数返回 `Err`。
             */
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

            /**
             * @brief 命令栏可识别的命令名与别名表。
             * @return 规范名与别名的有序列表（命令表序），供 did-you-mean 建议使用。
             * @note 与 `parse_command` 的分派面保持同步；simulate 作为可识别名字
             *       参与纠错，避免手误时无候选。
             */
            inline const std::vector<std::string> &command_names()
            {
                static const std::vector<std::string> names = {
                    "new",    "deal", "step", "run",    "r",     "status",
                    "st",     "save", "w",    "load",   "l",     "quit",
                    "q",      "help", "?",    "cards",  "rules", "audit",
                    "decks",  "heroes", "simulate"};
                return names;
            }

            /**
             * @brief  对未知命令名生成 did-you-mean 候选。
             * @param[in] token 用户输入的未知命令名（已 trim）。
             * @return 编辑距离不超过阈值的最近 1–3 个候选（距离升序、同距按命令
             *         表序）；无候选返回空。
             * @note   阈值 2 覆盖插入/删除/替换等常见手误；建议只用于提示，不参与
             *         命令分派。长度差即编辑距离下界，先据此跳过不可能命中的候选。
             */
            inline std::vector<std::string> suggest_commands(
                std::string_view token)
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

            /**
             * @brief  ASCII 大小写不敏感的子串查找。
             * @param[in] haystack 被查找文本（含中文时按字节原样比较）。
             * @param[in] needle   关键词；空串视为命中。
             * @return 命中返回 true。
             */
            inline bool contains_ci(std::string_view haystack,
                                    std::string_view needle)
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

            /**
             * @brief TUI 命令表/用法纯文本：命令+别名、只读查询、默认值、启动选项、键位。
             * @return 逐行文本（不含换行）；`Controller::do_help` 与启动 `--help/-h`
             *         共用同一来源，保证两入口逐行一致。
             * @note 默认值取自 Options/RulesConfig 声明，不写魔法数；事件日志恒开
             *       且无 --verbose 开关，在此明示避免与 CLI 混淆。
             */
            inline std::vector<std::string> help_lines()
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

            /**
             * @brief  按关键词过滤命令表。
             * @param[in] keyword 关键词；去空白后为空时返回未过滤的全量表（逐字节
             *                    与 `help_lines()` 一致）。
             * @return 命中行（ASCII 大小写不敏感子串匹配，中文按字节匹配）；
             *         无命中返回单行中文提示。
             * @note   过滤只挑选既有行、不重写文案，全量表断言不因本函数漂移；
             *         命令表仍由 `help_lines()` 单一维护。
             */
            inline std::vector<std::string> query_help_lines(
                std::string_view keyword)
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

        /**
         * @brief  解析一行命令文本。
         * @param[in] line 用户输入的单行文本（首尾空白忽略）。
         * @param[in] base 启动选项；new/deal 未显式给出的项沿用此基准。
         * @return `Ok(Command)`；`Err` 为面向用户的中文提示（未知命令/缺参/类型
         *         或越界/未知选项）。
         * @note   命令名与别名：run/r、status/st、save/w、load/l、quit/q、help/?。
         *         save/load 只取一个文件位置参数；new 只接受行内长选项与
         *         `--human`/`--no-human`/`--hero`（座位=武将，可重复，行内覆盖
         *         启动值），不接受位置参数；cards 接受可选 `--text` 与
         *         `--deck <路径>`，rules 接受至多一个关键词与 `--deck <路径>`，
         *         audit 接受 `--deck <路径>` 且无其它参数，decks/heroes 只接受
         *         `--deck <路径>`；help/? 接受至多一个关键词用于过滤命令表；
         *         simulate 接受 `<局数> [玩家数]` 与行内
         *         `--seed`/`--ai`/`--mode`/`--hand`/`--deck`，基种子缺省 1。
         *         未知命令名附邻近拼写建议。
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
                return CommandParseResult::Err("未知命令: '" + name +
                                               "'（help 查看用法）");

            std::string hint = "是否想输入: ";
            for (std::size_t i = 0; i < suggestions.size(); ++i)
            {
                if (i > 0)
                    hint += " / ";
                hint += suggestions[i];
            }
            hint += "？ / ";
            return CommandParseResult::Err("未知命令: '" + name + "'（" + hint +
                                           "help 查看用法）");
        }
    }  // namespace tui
}  // namespace tkw

#endif  // INCLUDE_TKW_TUI_COMMAND_HPP
