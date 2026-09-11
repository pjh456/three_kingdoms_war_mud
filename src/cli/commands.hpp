/**
 * @file commands.hpp
 * @brief CLI 命令树与命令执行体：声明公共选项、注册 11 个命令、共享 Session。
 * @note 与 main.cpp 分离，使命令树可由测试直接构建并驱动 REPL。命令的
 *       action 写标准输出（用户可见），框架侧输出（?/help）走 InteractiveConsole
 *       注入的流。
 */
#ifndef INCLUDE_TKW_CLI_COMMANDS_HPP
#define INCLUDE_TKW_CLI_COMMANDS_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <pjh_cli.hpp>
#include <pjh_cli/console/file_history.hpp>
#include <pjh_cli/console/help_navigator.hpp>
#include <pjh_cli/console/query_result.hpp>

#include "card/catalog.hpp"
#include "cli/error_zh.hpp"
#include "cli/help_zh.hpp"
#include "cli/render.hpp"
#include "cli/session.hpp"
#include "config/error.hpp"
#include "config/resource.hpp"
#include "entity/base.hpp"
#include "entity/event.hpp"
#include "entity/hp.hpp"
#include "event/handler.hpp"
#include "game/ai/aggressive.hpp"
#include "game/ai/human.hpp"
#include "game/ai/simple.hpp"
#include "game/core/card_event.hpp"
#include "game/flow/factory.hpp"
#include "game/flow/loop.hpp"
#include "game/flow/table.hpp"
#include "game/resolve/audit.hpp"
#include "io/file.hpp"
#include "save/reader.hpp"
#include "save/session_meta.hpp"
#include "save/writer.hpp"
#include "util/rng.hpp"
#include "util/types.hpp"

namespace tkw
{
    namespace cli
    {
        using pjh::cli::App;
        using pjh::cli::CliError;
        using pjh::cli::CliFailure;
        using pjh::cli::CliResult;
        using pjh::cli::ExtraArgsPolicy;
        using pjh::cli::fixed_string;
        using pjh::cli::InteractiveConsole;
        using pjh::cli::ParseContext;
        using pjh::cli::Visibility;

        namespace detail
        {
            /**
             * @brief 从解析上下文读参数；未出现的选项回落 base。
             * @note REPL 每行命令独立解析，根选项不会自动继承启动命令行的取值，
             *       故 REPL 内建局以启动选项为 base 合并，行内显式选项优先；真人
             *       座位是唯一可清空的累积项，--no-human 显式清空并优先于 --human。
             */
            inline Options options_from(ParseContext &ctx, const Options &base)
            {
                Options opt = base;
                opt.deck =
                    ctx.get_or<std::filesystem::path, fixed_string("deck")>(base.deck);
                opt.players = ctx.get_or<int, fixed_string("players")>(base.players);
                opt.hand = ctx.get_or<int, fixed_string("hand")>(base.hand);
                opt.seed = static_cast<std::uint32_t>(
                    ctx.get_or<int, fixed_string("seed")>(static_cast<int>(base.seed)));
                opt.verbose = ctx.get_or<bool, fixed_string("verbose")>(base.verbose);
                opt.ai = ctx.get_or_enum<AiLevel, fixed_string("ai")>(base.ai);
                opt.autosave =
                    ctx.get_or<std::filesystem::path, fixed_string("autosave")>(
                        base.autosave);
                opt.history =
                    ctx.get_or<std::filesystem::path, fixed_string("history")>(
                        base.history);
                if (ctx.get_or<bool, fixed_string("no-human")>(false))
                    opt.humans.clear();
                else if (ctx.has<fixed_string("human")>())
                    opt.humans = ctx.get_all<std::string, fixed_string("human")>();
                return opt;
            }

            /** 从解析上下文读参数（无 base：一次性命令使用选项默认值）。 */
            inline Options options_from(ParseContext &ctx)
            {
                return options_from(ctx, Options{});
            }

            /**
             * @brief 按选项构造 REPL 历史后端：空路径交回框架默认（内存）。
             * @param path 历史文件路径；空 = 不持久化。
             * @param err  告警输出流；父目录缺失时写一行中文告警。
             * @return FileHistory（父目录存在）或 nullptr（交 InteractiveConsole
             *         回落 InMemoryHistory）。
             * @note FileHistory 路径逐字使用、不建父目录，父目录缺失时写入会
             *       静默失败，故此处预检并显式告警，避免用户误以为已持久化。
             */
            inline std::unique_ptr<pjh::cli::IHistory> make_repl_history(
                const std::filesystem::path &path, std::ostream &err)
            {
                if (path.empty())
                    return nullptr;

                // 相对路径按进程 CWD 解析；裸文件名无父路径，跳过预检（CWD 必然存在）。
                if (path.has_parent_path() &&
                    !tkw::io::exists(path.parent_path()))
                {
                    err << "命令历史目录不存在，本次仅内存记录: " << path.string()
                        << "\n";
                    return nullptr;
                }

                return std::make_unique<pjh::cli::FileHistory>(path);
            }

            /**
             * @brief 在命令上声明标量公共选项（牌堆/人数/手牌/种子/日志/存档/历史/AI 难度）。
             * @param cmd   目标命令：根命令或会读取这些选项的 leaf。
             * @param rules 玩家数上下限来源。
             * @note pjh_cli 的选项查找沿父链（名与值都取最近声明处），故 leaf 不重
             *       声明也能解析祖先的选项；此处 per-leaf 重声明只为让 leaf 的帮助/
             *       用法行列出这些选项。未显式给的项仍由 options_from 沿父链或会话
             *       启动选项回落；标量「最近声明胜出」即期望语义，故 per-leaf 重声明
             *       无副作用。默认值只以描述文字标注，禁用 .default_value()：它会让
             *       parse_finalizer 每次解析把默认值写进上下文，压过 REPL 启动选项
             *       （session.base），破坏会话继承。--ai 走 enum 映射，值域外输入在
             *       解析期报 enum_value_error（与未知选项同一 rc=2 错误面）。
             */
            inline void declare_common_options(
                pjh::cli::BaseCommand &cmd, const tkw::game::RulesConfig &rules)
            {
                cmd.option<fixed_string("deck")>(
                       "--deck", 'd',
                       "资源目录（含 deck.json 与 cards/，默认 resources）")
                    .path();
                cmd.option<fixed_string("players")>(
                       "--players", 'p', "玩家数（2–8，默认 4）")
                    .integer()
                    .min(rules.min_players)
                    .max(rules.max_players);
                cmd.option<fixed_string("hand")>(
                       "--hand", "初始手牌数（默认 4）")
                    .integer()
                    .min(0)
                    .max(20);
                cmd.option<fixed_string("seed")>(
                       "--seed", 's', "随机种子（默认 42；simulate 基种子默认 1）")
                    .integer()
                    .min(0);
                cmd.option<fixed_string("verbose")>(
                       "--verbose", 'v',
                       "打印事件日志（摸牌/打出/弃置/移牌/伤害/体力/阵亡）")
                    .boolean();
                cmd.option<fixed_string("autosave")>(
                       "--autosave",
                       "REPL 退出时自动存档路径（默认 tkw-autosave.json，空串关闭）")
                    .path();
                cmd.option<fixed_string("history")>(
                       "--history",
                       "REPL 命令历史文件路径（默认不持久化，仅本次会话；父目录须已存在）")
                    .path();
                cmd.option<fixed_string("ai")>(
                       "--ai",
                       "AI 难度：simple 贪心 / aggressive 伤害优先（默认 simple）")
                    .enum_type<AiLevel>()
                    .mapping({{"simple", AiLevel::Simple},
                             {"aggressive", AiLevel::Aggressive}})
                    .completer([] {
                        // 候选值域与上方 enum 映射保持一致
                        return std::vector<std::string>{"simple", "aggressive"};
                    });
            }

            /**
             * @brief 在根命令上声明可重复的真人座位选项与清空开关。
             * @param cmd 目标命令；只应传根命令，使父/叶混写累积进同一上下文。
             * @note repeatable 选项的值按「最近声明」写入单一上下文，若根与 leaf 各
             *       声明一份，`--human P0 new --human P1` 会分落两处，而读取只取最近
             *       节点，导致 P0 静默丢弃；故仅根声明。leaf 处仍可解析（祖先链查找）。
             *       值补全候选取自规则允许的座位上限，防止越界座位号到运行期才报错；
             *       --no-human 是单独的布尔开关（非 --human 的取反），使 REPL 内能
             *       清空由启动选项带入的座位。
             */
            inline void declare_human_option(pjh::cli::BaseCommand &cmd)
            {
                cmd.option<fixed_string("human")>(
                       "--human",
                       "真人座位（可重复：--human P0 --human P2；存档不保存，读档后需重新指定）")
                    .str()
                    .repeatable()
                    .completer([] {
                        std::vector<std::string> seats;
                        const int max_players = tkw::game::RulesConfig{}.max_players;
                        for (int i = 0; i < max_players; ++i)
                            seats.push_back("P" + std::to_string(i));
                        return seats;
                    });
                cmd.option<fixed_string("no-human")>(
                       "--no-human", "清空真人座位（覆盖启动/会话 --human）")
                    .boolean();
            }

            /** 建局入参：只取装配所需字段（hand/AI/verbose 属会话参数）。 */
            inline tkw::game::BuildOptions build_options_from(const Options &opt)
            {
                return tkw::game::BuildOptions{opt.deck, opt.players, opt.seed};
            }

            /**
             * @brief 牌堆加载失败 → 用户可见文案（中文根因标签 + detail）。
             * @param e 目录加载错误；detail 为文件路径或字段路径。
             * @return 固定前缀「加载牌堆失败」+ 类别中文标签 + detail 的文案。
             */
            inline std::string format_load_error(const tkw::config::ConfigError &e)
            {
                return "加载牌堆失败（" +
                       std::string(config_error_kind_zh(e.kind)) + "）: " +
                       e.detail;
            }

            /**
             * @brief 对局失败 → 带中文根因标签的用户可见文案。
             * @param code 非 MaxRounds 的流程错误；达回合上限由调用方映射为平局。
             * @return 「对局失败（<标签>）」文案。
             * @note 仅一次性跑局与批量模拟使用；step/run 经根因出参走
             *       format_turn_failure，不经过本函数。
             */
            inline std::string format_loop_error(tkw::game::LoopError code)
            {
                return loop_error_zh(code);
            }

            /**
             * @brief 回合失败 → 带中文根因标签的用户可见文案。
             * @param code  step/run 循环返回的错误类别。
             * @param root  根因出参写回的回合错误；仅 `TurnFailed` 有效。
             * @param actor 失败回合的角色 id（失败时未推进，即当回合角色）。
             * @return 「回合执行失败（角色 <actor>，<标签>）」；`NoPlayers`
             *         表示会话角色已不存在，标签回落「角色不存在」。
             * @note 仅 `TurnFailed` 消费 `root`；`MaxRounds` 由调用方映射为平局，
             *       不进入本函数。
             */
            inline std::string format_turn_failure(
                tkw::game::LoopError code, tkw::game::TurnError root,
                const std::string &actor)
            {
                if (code == tkw::game::LoopError::NoPlayers)
                    return "回合执行失败（角色 " + actor + "，角色不存在）";
                return "回合执行失败（角色 " + actor + "，" +
                       std::string(turn_error_label_zh(root)) + "）";
            }

            /** 建局错误 → 用户可见文案（目录加载与玩家创建两类错误面）。 */
            inline std::string format_build_error(const tkw::game::BuildError &e)
            {
                if (e.kind == tkw::game::BuildError::Kind::CreatePlayer)
                    return "创建玩家失败: P" + std::to_string(e.player_index);
                return format_load_error(e.config);
            }

            /**
             * @brief 玩家数越界 → 用户可见文案。
             * @param value 实际传入的玩家数。
             * @param rules 玩家数上下限来源。
             * @return 「玩家数 N 超出范围 [min, max]」，与选项 --players 的解析期
             *         越界文案同用「超出范围 [min, max]」措辞。
             */
            inline std::string player_range_error(
                int value, const tkw::game::RulesConfig &rules)
            {
                return "玩家数 " + std::to_string(value) + " 超出范围 [" +
                       std::to_string(rules.min_players) + ", " +
                       std::to_string(rules.max_players) + "]";
            }

            /**
             * @brief 无进行中会话 → 用户可见错误文案（step/run/save 共用单点）。
             * @return 「没有进行中的对局（先运行 new 开局，或进入 tkw repl）」。
             * @note new/load 会建立会话，故引导指向新开局与 repl 两条入口；
             *       退出码由调用方维持 1。
             */
            inline const char *no_active_game_error()
            {
                return "没有进行中的对局（先运行 new 开局，或进入 tkw repl）";
            }

            /**
             * @brief 校验真人座位：必须是对局中存在的实体且互不重复；空串表示通过。
             * @return 查无此 id 时返回「真人座位不存在: <id>（可用座位: ...）」；
             *         重复时返回「真人座位重复: <id>（每个座位只能指定一次）」；
             *         全部通过返回空串。
             * @note 可用座位取当前存活实体的按座位序 id 列表。读档后阵亡者不在
             *       实体集合中，故列表如实反映此刻可选座位，而非 P0..P{n-1} 范围。
             */
            inline std::string validate_humans(
                tkw::game::Game &game, const std::vector<std::string> &humans)
            {
                auto ctx = game.context();
                const auto seats = ctx.entities->ordered_ids();
                const std::string seat_hint =
                    seats.empty() ? "（无可用座位）"
                                  : "（可用座位: " + join_items(seats, "、") + "）";
                for (std::size_t i = 0; i < humans.size(); ++i)
                {
                    if (ctx.entities->find(humans[i]).is_none())
                        return "真人座位不存在: " + humans[i] + seat_hint;
                    for (std::size_t j = i + 1; j < humans.size(); ++j)
                        if (humans[i] == humans[j])
                            return "真人座位重复: " + humans[i] +
                                   "（每个座位只能指定一次）";
                }
                return {};
            }

            /** 不支持真人的命令统一拒绝非空 humans；返回空串表示通过。 */
            inline std::string reject_humans(
                const std::vector<std::string> &humans, const std::string &cmd)
            {
                if (humans.empty())
                    return {};
                return cmd + " 不支持 --human（该命令不运行真人参与的对局）";
            }

            /** AI 难度档 → 命令行/存档值域字符串。 */
            inline const char *ai_level_name(AiLevel ai)
            {
                return ai == AiLevel::Aggressive ? "aggressive" : "simple";
            }

            /** 值域字符串 → AI 难度档；未知或空串返回 None，由调用方回落默认档。 */
            inline tkw::Option<AiLevel> ai_level_from(std::string_view name)
            {
                if (name == "simple")
                    return tkw::Option<AiLevel>::Some(AiLevel::Simple);
                if (name == "aggressive")
                    return tkw::Option<AiLevel>::Some(AiLevel::Aggressive);
                return tkw::Option<AiLevel>::None();
            }

            /**
             * @brief 构造决策源：无真人按难度档取 AI，否则按 actor 路由到交互输入
             *        （真人座位外的回落与全 AI 局同一难度档）。
             * @param humans 真人座位 id；空 = 全 AI 对局。
             * @param ai     AI 难度档（决定全 AI 局与真人局回落决策源）。
             */
            inline std::unique_ptr<tkw::game::DecisionSource> make_decision_source(
                const std::vector<std::string> &humans, AiLevel ai)
            {
                auto make_ai = [](AiLevel lvl) -> std::unique_ptr<tkw::game::DecisionSource>
                {
                    if (lvl == AiLevel::Aggressive)
                        return std::make_unique<tkw::game::AggressiveAI>();
                    return std::make_unique<tkw::game::SimpleAI>();
                };
                if (humans.empty())
                    return make_ai(ai);
                return std::make_unique<tkw::game::ai::RoutedAI>(
                    humans, std::cin, std::cout, make_ai(ai));
            }

            /** @brief 跑到底结果：正常结束或达回合上限平局。 */
            enum class RunOutcome : std::uint8_t
            {
                Finished,  /**< 会话结束（存活 ≤ 1） */
                MaxRounds, /**< 达回合上限且无唯一存活者 */
            };

            /**
             * @brief 重复 step_session 直到会话结束或达回合上限。
             * @param ctx     对局运行时；结束判定与逐步推进都作用于其容器。
             * @param ai      决策源，由调用方按真人/AI 档构造。
             * @param session 会话进度，原地推进。
             * @param root    非空时透传给 step_session，在回合失败时写回根因。
             * @return Ok(Finished) 会话结束；Ok(MaxRounds) 达回合上限平局；
             *         Err 其它 LoopError 原样上抛。
             * @note 只驱动循环，不订阅事件、不打印；会话状态与统计由调用方持有。
             */
            inline tkw::game::LoopResult<RunOutcome> run_to_completion(
                tkw::game::GameContext &ctx, tkw::game::DecisionSource &ai,
                tkw::game::GameSession &session,
                tkw::game::TurnError *root = nullptr)
            {
                while (!tkw::game::session_over(ctx))
                {
                    auto r = tkw::game::step_session(ctx, ai, session, root);
                    if (r.is_ok())
                        continue;
                    if (r.unwrap_err() == tkw::game::LoopError::MaxRounds)
                        return tkw::game::LoopResult<RunOutcome>::Ok(
                            RunOutcome::MaxRounds);
                    return tkw::game::LoopResult<RunOutcome>::Err(r.unwrap_err());
                }
                return tkw::game::LoopResult<RunOutcome>::Ok(RunOutcome::Finished);
            }

            /**
             * @brief 打印会话状态：无会话 / 进行中 / 已结束三态。
             * @param s 当前会话；active 为假或 game 为空时只打印「会话: 无」。
             * @note 结束态以引擎 session_over（存活 ≤ 1）判定，胜者经 winner_label
             *       回落，0 存活显示「平局（同归于尽）」；仅进行中打印「下一回合」。
             */
            inline void print_status(const Session &s)
            {
                if (!s.active || !s.game)
                {
                    std::cout << "会话: 无\n";
                    return;
                }

                auto ctx = s.game->context();
                const bool over = tkw::game::session_over(ctx);
                std::cout << "会话: " << (over ? "已结束" : "进行中") << "\n";

                // 已结束不再提示下一回合，与 run/deal 共用 winner_label 回落。
                if (over)
                    std::cout << "  胜者: " << winner_label(tkw::game::session_winner(ctx))
                              << "，已执行回合: " << s.state.turns
                              << "，存活: " << ctx.entities->size() << "\n";
                else
                    std::cout << "  下一回合: " << s.state.current
                              << "，已执行回合: " << s.state.turns
                              << "，存活: " << ctx.entities->size() << "\n";

                std::cout << "  AI 难度: " << ai_level_name(s.ai) << "\n";
                std::cout << "  真人座位: ";
                if (s.humans.empty())
                    std::cout << "无";
                else
                    for (std::size_t i = 0; i < s.humans.size(); ++i)
                        std::cout << (i == 0 ? "" : ",") << s.humans[i];
                std::cout << "\n";
                std::cout << "  局面:\n";
                for (const auto &e : *ctx.entities)
                    std::cout << "    " << e->get_id() << " 体力 " << e->get_hp() << "/"
                              << e->get_hp_bar().get_max() << " 手牌 "
                              << ctx.cards->hand_size(e->get_id()) << " 装备 "
                              << ctx.cards->equip_size(e->get_id()) << " 判定 "
                              << ctx.cards->judge_size(e->get_id()) << "\n";
            }

            /**
             * @brief 打印回合上限平局行与统计块（循环尾同构两连）。
             * @param stats 本局统计聚合。
             * @param game  本局运行时；统计块读取实体体力。
             * @param turns 已执行回合数。
             * @note 平局无胜者，统计块 winner 传空串（显示「无」）。
             */
            inline void print_max_rounds_draw(
                const tkw::save::BattleStats &stats, const tkw::game::Game &game,
                int turns)
            {
                std::cout << "平局（达到最大回合数）\n";
                print_battle_stats(stats, game, {}, turns);
            }

            inline CliResult<void> cmd_new(const Options &opt, Session &s)
            {
                auto built = tkw::game::build_game(build_options_from(opt));
                if (built.is_err())
                    return CliFailure{CliError(format_build_error(built.unwrap_err()))};
                auto game = std::move(built).unwrap();
                const std::string verr = validate_humans(*game, opt.humans);
                if (!verr.empty())
                    return CliFailure{CliError(verr)};
                auto log = subscribe_event_log(*game, opt.verbose);
                auto ctx = game->context();
                tkw::game::GameSession state;
                if (tkw::game::start_session(ctx, state, "P0", opt.hand).is_err())
                    return CliFailure{CliError("开局失败")};
                s.game = std::move(game);
                s.state = std::move(state);
                s.humans = opt.humans;
                s.ai = opt.ai;
                s.verbose = opt.verbose;
                s.active = true;
                s.stats = BattleStats{};
                std::cout << "新对局已开始\n";
                print_status(s);
                return CliResult<void>::Ok();
            }

            inline CliResult<void> cmd_step(Session &s, bool verbose)
            {
                if (!s.active || !s.game)
                    return CliFailure{CliError(no_active_game_error())};
                auto log = subscribe_event_log(*s.game, verbose);
                auto stats_handles = subscribe_stats(*s.game, s.stats);
                auto ai = make_decision_source(s.humans, s.ai);
                auto ctx = s.game->context();
                if (tkw::game::session_over(ctx))
                {
                    std::cout << "对局已结束，胜者: "
                              << winner_label(tkw::game::session_winner(ctx))
                              << "\n";
                    return CliResult<void>::Ok();
                }
                tkw::game::TurnError root = tkw::game::TurnError::PlayRejected;
                auto r = tkw::game::step_session(ctx, *ai, s.state, &root);
                if (r.is_err())
                {
                    if (r.unwrap_err() == tkw::game::LoopError::MaxRounds)
                    {
                        print_max_rounds_draw(s.stats, *s.game, s.state.turns);
                        return CliResult<void>::Ok();
                    }
                    return CliFailure{CliError(
                        format_turn_failure(r.unwrap_err(), root, s.state.current))};
                }
                if (tkw::game::session_over(ctx))
                {
                    std::cout << "对局结束，胜者: "
                              << winner_label(tkw::game::session_winner(ctx))
                              << "\n";
                    print_battle_stats(
                        s.stats, *s.game, tkw::game::session_winner(ctx), s.state.turns);
                }
                else
                    print_status(s);
                return CliResult<void>::Ok();
            }

            inline CliResult<void> cmd_run(Session &s, bool verbose)
            {
                if (!s.active || !s.game)
                    return CliFailure{CliError(no_active_game_error())};
                auto log = subscribe_event_log(*s.game, verbose);
                auto stats_handles = subscribe_stats(*s.game, s.stats);
                auto ai = make_decision_source(s.humans, s.ai);
                auto ctx = s.game->context();
                // 进入循环前判定：true 表示本次命令至少会推进（用于末尾统计门控）。
                const bool advanced = !tkw::game::session_over(ctx);
                tkw::game::TurnError root = tkw::game::TurnError::PlayRejected;
                auto rr = run_to_completion(ctx, *ai, s.state, &root);
                if (rr.is_err())
                    return CliFailure{CliError(
                        format_turn_failure(rr.unwrap_err(), root, s.state.current))};
                if (rr.unwrap() == RunOutcome::MaxRounds)
                {
                    print_max_rounds_draw(s.stats, *s.game, s.state.turns);
                    return CliResult<void>::Ok();
                }
                std::cout << "胜者: "
                          << winner_label(tkw::game::session_winner(ctx))
                          << "，回合数: " << s.state.turns << "\n";
                // 对局在本次命令内跑完才附统计块；已在更早 step 结束时不重复打印。
                if (advanced)
                    print_battle_stats(
                        s.stats, *s.game, tkw::game::session_winner(ctx), s.state.turns);
                return CliResult<void>::Ok();
            }

            /**
             * @brief 序列化当前会话（含 AI 档与对局统计）并原子写入文件。
             * @return Err 无进行中会话 / 写文件失败；成功返回 Ok。
             */
            inline CliResult<void> cmd_save(
                const std::filesystem::path &file, Session &s)
            {
                if (!s.active || !s.game)
                    return CliFailure{CliError(no_active_game_error())};
                tkw::save::SessionMeta meta;
                meta.ai = ai_level_name(s.ai);
                meta.stats = s.stats;
                const std::string text =
                    tkw::save::write(*s.game, s.state, "deck", meta);
                const auto write = tkw::io::write_text_atomic(file, text);
                if (write.is_err())
                    return CliFailure{
                        CliError(render_write_error_zh(file, write.unwrap_err()))};
                std::cout << "已保存: " << file.string() << "\n";
                return CliResult<void>::Ok();
            }

            /**
             * @brief 读档并落子到会话：恢复存档 AI 档与统计，verbose 不持久化。
             * @param ai_explicit 命令行是否显式给了 --ai；显式值覆盖存档 AI 档。
             * @return Err 读文件 / 存档解析 / 真人座位校验失败；成功返回 Ok。
             * @note 旧档无元数据时 AI 档回落命令行取值、统计为空；未知 AI 文本
             *       亦回落命令行，不拒绝存档。
             */
            inline CliResult<void> cmd_load(
                const Options &opt, const std::filesystem::path &file, Session &s,
                bool ai_explicit)
            {
                auto text = tkw::io::read_text(file);
                if (text.is_err())
                    return CliFailure{
                        CliError(render_read_error_zh(file, text.unwrap_err()))};
                auto built = tkw::game::build_game(build_options_from(opt));
                if (built.is_err())
                    return CliFailure{CliError(format_build_error(built.unwrap_err()))};
                auto game = std::move(built).unwrap();
                tkw::game::GameSession state;
                tkw::save::SessionMeta meta;
                auto r = tkw::save::read(text.unwrap(), *game, state, &meta);
                if (r.is_err())
                    return CliFailure{CliError(render_save_error_zh(r.unwrap_err()))};
                const std::string verr = validate_humans(*game, opt.humans);
                if (!verr.empty())
                    return CliFailure{CliError(verr)};
                s.game = std::move(game);
                s.state = std::move(state);
                s.humans = opt.humans;
                if (!meta.ai.empty() && !ai_explicit)
                {
                    if (auto lvl = ai_level_from(meta.ai); lvl.is_some())
                        s.ai = lvl.unwrap();
                    else
                        s.ai = opt.ai;
                }
                else
                    s.ai = opt.ai;
                s.stats = std::move(meta.stats);
                s.verbose = opt.verbose;
                s.active = true;
                std::cout << "已加载: " << file.string() << "\n";
                print_status(s);
                return CliResult<void>::Ok();
            }

            inline CliResult<void> run_game(const Options &opt)
            {
                auto built = tkw::game::build_game(build_options_from(opt));
                if (built.is_err())
                    return CliFailure{CliError(format_build_error(built.unwrap_err()))};
                auto game = std::move(built).unwrap();

                // 此处只覆盖「枚举已存在但结算未实现」；未知机制名在严格加载期
                // 即失败，不会到达这里（未知机制的审计报告见 audit 子命令）。
                const auto unsupported =
                    tkw::game::unsupported_cards(game->catalog);
                if (!unsupported.empty())
                {
                    std::cerr << "警告: 牌堆含 " << unsupported.size()
                              << " 张引擎未实现的卡:";
                    for (const auto &id : unsupported)
                        std::cerr << ' ' << audit_entry_name(game->catalog, id);
                    std::cerr << "\n";
                }

                const std::string verr = validate_humans(*game, opt.humans);
                if (!verr.empty())
                    return CliFailure{CliError(verr)};

                auto ctx = game->context();
                auto log = subscribe_event_log(*game, opt.verbose);
                BattleStats stats;
                auto stats_handles = subscribe_stats(*game, stats);

                auto ai = make_decision_source(opt.humans, opt.ai);
                tkw::game::GameSession session;
                if (tkw::game::start_session(ctx, session, "P0", opt.hand).is_err())
                    return CliFailure{CliError("开局失败")};
                auto rr = run_to_completion(ctx, *ai, session);
                if (rr.is_err())
                    return CliFailure{CliError(format_loop_error(rr.unwrap_err()))};
                if (rr.unwrap() == RunOutcome::MaxRounds)
                {
                    print_max_rounds_draw(stats, *game, session.turns);
                    return CliResult<void>::Ok();
                }
                const std::string winner = tkw::game::session_winner(ctx);
                std::cout << "胜者: " << winner_label(winner)
                          << "，回合数: " << session.turns << "\n";
                print_battle_stats(stats, *game, winner, session.turns);
                return CliResult<void>::Ok();
            }

            inline CliResult<void> audit_deck(const Options &opt)
            {
                const std::string herr = reject_humans(opt.humans, "audit");
                if (!herr.empty())
                    return CliFailure{CliError(herr)};

                // 不建局：只按原始机制名审计，未知机制名逐卡列出而非整体拒载
                // （deal/simulate 仍走严格加载，未知机制在建局入口响亮失败）。
                tkw::config::ResourceStore store(opt.deck);
                auto unsupported = tkw::game::unsupported_cards(store, "deck");
                if (unsupported.is_err())
                {
                    const auto &e = unsupported.unwrap_err();
                    return CliFailure{CliError(format_load_error(e))};
                }
                const auto &cards = unsupported.unwrap();
                if (cards.empty())
                {
                    std::cout << "牌堆全部可结算\n";
                    return CliResult<void>::Ok();
                }
                std::cout << "未实现卡（" << cards.size() << " 张）:\n";
                for (const auto &c : cards)
                    std::cout << "  " << c.name << "(" << c.id << ")\n";
                return CliResult<void>::Ok();
            }

            /**
             * @brief 列出牌表：只加载卡牌目录，打印牌堆名与种类/张数，再按
             *        deck 序逐卡打印「中文名(id) 大类 张数」。
             * @return Ok；Err 为牌堆加载失败（kind + detail，与建局错误面一致）。
             * @note 只读牌堆查询，不建局、不消耗随机源；公共选项中仅 --deck
             *       生效，其余被接受但不读取（与 audit 声明面一致），--human
             *       因该命令不运行对局而被拒绝。deck.json
             *       的 name 缺失或类型不符时头行退化为无牌堆名，不阻断列出。
             */
            inline CliResult<void> cards_list(const Options &opt)
            {
                const std::string herr = reject_humans(opt.humans, "cards");
                if (!herr.empty())
                    return CliFailure{CliError(herr)};
                tkw::config::ResourceStore store(opt.deck);
                auto catalog = tkw::card::CardDefCatalog::load(store, "deck");
                if (catalog.is_err())
                {
                    const auto &e = catalog.unwrap_err();
                    return CliFailure{CliError(format_load_error(e))};
                }
                const auto &cat = catalog.unwrap();

                std::string deck_name;
                const auto deck_doc = store.load("deck");
                if (deck_doc.is_ok())
                {
                    const auto nm = tkw::config::opt_string(
                        deck_doc.unwrap().root(), "name", "", "deck");
                    if (nm.is_ok())
                        deck_name = nm.unwrap();
                }

                std::cout << "牌堆" << (deck_name.empty() ? "" : " " + deck_name)
                          << "（" << cat.size() << " 种 / " << cat.total_copies()
                          << " 张）\n";
                for (const auto &def : cat)
                    std::cout << "  " << def.name << "(" << def.id << ") "
                              << card_type_zh(def.type) << ' ' << def.copies.size()
                              << "\n";
                return CliResult<void>::Ok();
            }

            /** 跨局模拟聚合：各座位胜场、平局局数与回合总和（单局展示统计不可跨局累加）。 */
            struct SimAggregate
            {
                std::map<std::string, int> wins; /**< 座位 id → 胜场数 */
                int draws = 0;                  /**< 平局局数（达回合上限或同归于尽） */
                std::int64_t turns_sum = 0;      /**< 全部局回合数总和 */
            };

            /**
             * @brief 批量模拟：N 局独立种子全 AI 跑完，打印跨局聚合摘要（胜者
             *        分布 / 平局 / 平均回合）。
             * @param opt 对局选项；seed 为基种子（第 i 局用 seed + i），
             *        --deck/--players/--hand/--seed/--ai 生效。
             * @param n   局数；须 ≥1（由调用方校验），耗时随 n 线性。
             * @return Ok；Err 为牌堆加载失败 / 开局失败 / 对局失败（文案与
             *         run_game 一致）。
             * @note 基种子缺省 1（局种子 1..N，跨档对比口径可比），与
             *       Options.seed 缺省 42 不同；--verbose/--autosave 接受但不读
             *       （逐局不打事件日志、无会话），--human 因全 AI 批量模拟而拒绝。
             *       胜者空串 = 平局；平均回合为
             *       回合总和除以局数（向下取整）。每局不打印胜者行与统计块，
             *       未实现卡警告在循环前打印一次。
             */
            inline CliResult<void> simulate_games(const Options &opt, int n)
            {
                const std::string herr = reject_humans(opt.humans, "simulate");
                if (!herr.empty())
                    return CliFailure{CliError(herr)};
                // 预载牌表：未实现卡警告循环前打一次，不逐局重复。
                tkw::config::ResourceStore store(opt.deck);
                auto catalog = tkw::card::CardDefCatalog::load(store, "deck");
                if (catalog.is_err())
                {
                    const auto &e = catalog.unwrap_err();
                    return CliFailure{CliError(format_load_error(e))};
                }
                const auto &cat = catalog.unwrap();
                // 同 run_game：未知机制名到不了这里，非空只在未来枚举实现未补时出现。
                const auto unsupported = tkw::game::unsupported_cards(cat);
                if (!unsupported.empty())
                {
                    std::cerr << "警告: 牌堆含 " << unsupported.size()
                              << " 张引擎未实现的卡:";
                    for (const auto &id : unsupported)
                        std::cerr << ' ' << audit_entry_name(cat, id);
                    std::cerr << "\n";
                }

                SimAggregate agg;
                for (int i = 0; i < n; ++i)
                {
                    // 每局独立随机源：种子 = 基种子 + 局序号。
                    Options per = opt;
                    per.seed = opt.seed + static_cast<std::uint32_t>(i);
                    auto built = tkw::game::build_game(build_options_from(per));
                    if (built.is_err())
                        return CliFailure{
                            CliError(format_build_error(built.unwrap_err()))};
                    auto game = std::move(built).unwrap();

                    // 全 AI 局：无真人座位，决策源按难度档取单档。
                    auto ctx = game->context();
                    auto ai = make_decision_source({}, opt.ai);
                    tkw::game::GameSession session;
                    if (tkw::game::start_session(ctx, session, "P0",
                                                  opt.hand).is_err())
                        return CliFailure{CliError("开局失败")};
                    auto rr = run_to_completion(ctx, *ai, session);
                    if (rr.is_err())
                        return CliFailure{CliError(format_loop_error(rr.unwrap_err()))};

                    // 胜者空串 = 平局（达回合上限或同归于尽）。
                    const std::string winner = tkw::game::session_winner(ctx);
                    if (winner.empty())
                        ++agg.draws;
                    else
                        ++agg.wins[winner];
                    agg.turns_sum += session.turns;
                }

                // 汇总：头行（局数/人数/种子区间/AI 档）+ 逐座位胜场与平局 + 平均回合。
                std::cout << "模拟 " << n << " 局（" << opt.players << " 人，种子 "
                          << opt.seed << ".." << opt.seed + n - 1 << "，ai="
                          << ai_level_name(opt.ai) << "）:\n";
                std::string line;
                for (int seat = 0; seat < opt.players; ++seat)
                {
                    const std::string id = "P" + std::to_string(seat);
                    const auto it = agg.wins.find(id);
                    const int w = it == agg.wins.end() ? 0 : it->second;
                    line += (seat == 0 ? "" : "，") + id + " 胜 " +
                            std::to_string(w);
                }
                line += "，平局 " + std::to_string(agg.draws);
                std::cout << "  " << line << "\n";
                std::cout << "  平均回合 " << (agg.turns_sum / n) << "\n";
                return CliResult<void>::Ok();
            }

        }  // namespace detail

        /**
         * @brief 构建完整命令树并绑定会话。
         * @param app     根命令（App）；额外参数策略与中文帮助在此一并设置。
         * @param session 跨命令会话，须比 app 的命令存活更久（action 以引用捕获）。
         * @note 命令树与 main() 分离，使测试可构建同一棵树并驱动 InteractiveConsole；
         *       根命令无子命令时跑一局，各 leaf 声明自身可读选项。
         */
        inline void build_app(pjh::cli::App &app, Session &session)
        {
            app.set_help_formatter(
                [](const pjh::cli::BaseCommand &cmd) { return render_help_zh(cmd); });

            const tkw::game::RulesConfig rules{};

            // 根命令选项（无子命令时直接跑一局）；各 leaf 各自声明一份标量选项，
            // 使帮助/用法面与本命令选项段一致，且子命令名之后的选项也可解析；
            // --human 是 repeatable，仅根声明以避免父/叶混写时值分落两处。
            detail::declare_common_options(app, rules);
            detail::declare_human_option(app);

            // 根命令：无子命令时直接跑一局 AI 对局，先打印一行引导。
            app.action(
                [](ParseContext &ctx) -> CliResult<void>
                {
                    std::cout
                        << "（无子命令：跑一局 AI 对局；--help 查看命令，repl 进入交互，"
                           "--human P0 真人参与）\n";
                    return detail::run_game(detail::options_from(ctx));
                });

            // audit：审计牌堆
            auto &audit = app.add_leaf("audit", "审计牌堆，列出引擎未实现的卡");
            detail::declare_common_options(audit, rules);
            audit.action(
                [](ParseContext &ctx) -> CliResult<void>
                { return detail::audit_deck(detail::options_from(ctx)); });

            // cards：列出牌表（只读牌堆查询，仅 --deck 生效；公共选项与 audit 同款）
            auto &cards = app.add_leaf("cards", "列出牌表（牌堆种类与张数）");
            detail::declare_common_options(cards, rules);
            cards.action(
                [](ParseContext &ctx) -> CliResult<void>
                { return detail::cards_list(detail::options_from(ctx)); });

            // deal：位置参数跑局（REPL/批量通用）
            auto &deal = app.add_leaf("deal", "跑一局：deal <玩家数> <种子>");
            detail::declare_common_options(deal, rules);
            deal.arg<int, 0>("players", "玩家数").required();
            deal.arg<int, 1>("seed", "随机种子").required();
            deal.action(
                [rules](ParseContext &ctx) -> CliResult<void>
                {
                    Options opt = detail::options_from(ctx);
                    opt.players = ctx.get<int, 0>();
                    opt.seed = static_cast<std::uint32_t>(ctx.get<int, 1>());
                    if (opt.players < rules.min_players ||
                        opt.players > rules.max_players)
                        return CliFailure{
                            CliError(detail::player_range_error(opt.players, rules))};
                    return detail::run_game(opt);
                });

            // simulate：批量模拟（全 AI 跨局聚合；玩家数缺省 4，可被 --players 覆盖）
            auto &sim = app.add_leaf(
                "simulate", "批量模拟：simulate <局数> [玩家数]");
            detail::declare_common_options(sim, rules);
            sim.arg<int, 0>("n", "局数（≥1）").required();
            sim.arg<int, 1>("players", "玩家数（可选，默认 4）");
            sim.action(
                [rules](ParseContext &ctx) -> CliResult<void>
                {
                    const int n = ctx.get<int, 0>();
                    if (n < 1)
                        return CliFailure{CliError("局数须为正整数")};
                    Options opt = detail::options_from(ctx);
                    opt.players = ctx.get_or<int, 1>(opt.players);
                    if (opt.players < rules.min_players ||
                        opt.players > rules.max_players)
                        return CliFailure{
                            CliError(detail::player_range_error(opt.players, rules))};
                    // --seed 按 Options 缺省为 42；simulate 的基种子缺省 1（局种子 1..N）
                    opt.seed = static_cast<std::uint32_t>(
                        ctx.get_or<int, fixed_string("seed")>(1));
                    return detail::simulate_games(opt, n);
                });

            // new：开新对局（不立即跑），供 step/run/save 续用
            auto &new_cmd =
                app.add_leaf("new", "开新对局（用 --players/--seed/--hand）");
            detail::declare_common_options(new_cmd, rules);
            new_cmd.action(
                [&session](ParseContext &ctx) -> CliResult<void>
                {
                    return detail::cmd_new(
                        detail::options_from(ctx, session.base), session);
                });

            // step：执行一个回合
            auto &step_cmd = app.add_leaf("step", "执行当前会话的一个回合");
            detail::declare_common_options(step_cmd, rules);
            step_cmd.action(
                [&session](ParseContext &ctx) -> CliResult<void>
                {
                    return detail::cmd_step(
                        session,
                        session.verbose ||
                            ctx.get_or<bool, fixed_string("verbose")>(false));
                });

            // run：跑到对局结束（别名 r，REPL 会话流高频命令）
            auto &run_cmd = app.add_leaf("run", "跑到当前会话结束");
            run_cmd.alias("r");
            detail::declare_common_options(run_cmd, rules);
            run_cmd.action(
                [&session](ParseContext &ctx) -> CliResult<void>
                {
                    return detail::cmd_run(
                        session,
                        session.verbose ||
                            ctx.get_or<bool, fixed_string("verbose")>(false));
                });

            // status：查看会话状态（别名 st）
            auto &status_cmd = app.add_leaf("status", "查看当前会话状态");
            status_cmd.alias("st");
            detail::declare_common_options(status_cmd, rules);
            status_cmd.action(
                [&session](ParseContext &) -> CliResult<void>
                {
                    detail::print_status(session);
                    return CliResult<void>::Ok();
                });

            // save：保存当前对局（别名 w）
            auto &save_cmd = app.add_leaf("save", "保存当前对局：save <file>");
            save_cmd.alias("w");
            detail::declare_common_options(save_cmd, rules);
            save_cmd.arg<std::string, 0>("file", "存档路径").required();
            save_cmd.action(
                [&session](ParseContext &ctx) -> CliResult<void>
                {
                    return detail::cmd_save(ctx.get<std::string, 0>(), session);
                });

            // load：从存档继续（别名 l）
            auto &load_cmd = app.add_leaf("load", "加载存档：load <file>");
            load_cmd.alias("l");
            detail::declare_common_options(load_cmd, rules);
            load_cmd.arg<std::string, 0>("file", "存档路径").required();
            load_cmd.action(
                [&session](ParseContext &ctx) -> CliResult<void>
                {
                    return detail::cmd_load(
                        detail::options_from(ctx, session.base),
                        ctx.get<std::string, 0>(), session,
                        ctx.has<fixed_string("ai")>());
                });

            // repl：交互模式（对局即 MUD 方向）
            auto &repl =
                app.add_leaf("repl", "进入交互模式（? 查看命令，quit 退出）");
            detail::declare_common_options(repl, rules);
            repl.set_visibility(Visibility::Cli);
            repl.action(
                [&app, &session](ParseContext &ctx) -> CliResult<void>
                {
                    session.base = detail::options_from(ctx);
                    const Options &opt = session.base;
                    // 进入 REPL 前打印引导：命令列表与退出方式在提示符处不可见。
                    std::cout << "输入 ? 查看命令，help <命令> 看用法，quit 退出\n";
                    InteractiveConsole console(
                        app, "tkw> ", std::cin, std::cout, std::cerr,
                        [&app](const pjh::cli::QueryResult &r)
                        { return render_query_zh(app, r); },
                        [](const pjh::cli::HelpNavigationResult &r)
                        { return render_help_nav_zh(r); },
                        detail::make_repl_history(opt.history, std::cerr));
                    console.set_error_formatter(render_error_zh);
                    console.run();
                    if (session.active && session.game && !opt.autosave.empty())
                    {
                        tkw::save::SessionMeta meta;
                        meta.ai = detail::ai_level_name(session.ai);
                        meta.stats = session.stats;
                        const std::string text = tkw::save::write(
                            *session.game, session.state, "deck", meta);
                        if (tkw::io::write_text_atomic(opt.autosave, text).is_ok())
                            std::cout << "已自动存档: " << opt.autosave.string()
                                      << "\n";
                        else
                            // 静默失败会让用户下次 load 才发现丢档，必须显式提示
                            std::cerr << "自动存档失败: " << opt.autosave.string()
                                      << "\n";
                    }
                    return CliResult<void>::Ok();
                });

            // 根不显式设策略，未知命令才落到框架的 unknown command 提示；各子命令
            // 显式设为 Error，保证子命令名之后的多余参数仍报错而非静默丢弃。
            for (auto &sub : app.subcommands())
                sub->set_extra_args(ExtraArgsPolicy::Error);
        }
    }  // namespace cli
}  // namespace tkw

#endif  // INCLUDE_TKW_CLI_COMMANDS_HPP
