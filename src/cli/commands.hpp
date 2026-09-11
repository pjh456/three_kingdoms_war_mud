/**
 * @file commands.hpp
 * @brief CLI 命令树与命令执行体：声明公共选项、注册 10 个命令、共享 Session。
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
#include <pjh_cli/console/help_navigator.hpp>
#include <pjh_cli/console/query_result.hpp>

#include "card/catalog.hpp"
#include "config/error.hpp"
#include "config/resource.hpp"
#include "entity/base.hpp"
#include "entity/event.hpp"
#include "entity/hp.hpp"
#include "event/handler.hpp"
#include "game/ai/aggressive.hpp"
#include "game/ai/human.hpp"
#include "game/ai/simple.hpp"
#include "game/resolve/audit.hpp"
#include "game/core/card_event.hpp"
#include "game/flow/loop.hpp"
#include "game/flow/table.hpp"
#include "io/file.hpp"
#include "save/reader.hpp"
#include "save/writer.hpp"
#include "util/rng.hpp"

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

        /** AI 难度档（CLI 值域；引擎侧实现为 SimpleDecider / AggressiveDecider）。 */
        enum class AiLevel : std::uint8_t
        {
            Simple,     /**< 贪心档（默认） */
            Aggressive, /**< 攻击优先档（伤害/多目标先行） */
        };

        /** 命令行/REPL 解析出的对局参数。 */
        struct Options
        {
            std::filesystem::path deck = "resources";
            int players = 4;
            int hand = tkw::game::RulesConfig{}.initial_hand;
            std::uint32_t seed = 42;
            bool verbose = false;
            std::filesystem::path autosave = "tkw-autosave.json";
            std::vector<std::string> humans; /**< 真人座位 id（可重复选项累积） */
            AiLevel ai = AiLevel::Simple;    /**< AI 难度档（默认 simple，零行为变化） */
        };

        /** 对局统计聚合：按已发布事件累计伤害/治疗与击杀归属，供对局结束复盘打印。 */
        struct BattleStats
        {
            std::map<std::string, int> damage_dealt;             /**< 来源 → 造成伤害总量 */
            std::map<std::string, int> healing;                  /**< 目标 → 恢复总量 */
            std::map<std::string, int> kills;                    /**< 击杀者 → 击杀数 */
            std::map<std::string, std::string> last_hit_source;  /**< 受害者 → 最近一次伤害来源 */
            std::set<std::string> died;                          /**< 本局阵亡实体 id */
        };

        /** 跨命令持有的对局会话（new/step/run/save/load 共享）。 */
        struct Session
        {
            std::unique_ptr<tkw::game::Game> game;
            tkw::game::GameSession state;
            std::vector<std::string> humans; /**< 本会话的真人座位 id */
            AiLevel ai = AiLevel::Simple;    /**< 本会话 AI 难度（new/load 写入，step/run 消费） */
            bool verbose = false;            /**< 本会话是否打印事件日志 */
            Options base;                    /**< REPL 启动选项（供行内命令继承） */
            bool active = false;
            BattleStats stats; /**< 本会话累计的对局统计（new/load 时重置；对局结束时打印） */
        };

        namespace detail
        {
            /**
             * @brief 从解析上下文读参数；未出现的选项回落 base。
             * @note REPL 每行命令独立解析，根选项不会自动继承启动命令行的取值，
             *       故 REPL 内建局以启动选项为 base 合并，行内显式选项优先。
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
                if (ctx.has<fixed_string("human")>())
                    opt.humans = ctx.get_all<std::string, fixed_string("human")>();
                return opt;
            }

            /** 从解析上下文读参数（无 base：一次性命令使用选项默认值）。 */
            inline Options options_from(ParseContext &ctx)
            {
                return options_from(ctx, Options{});
            }

            /**
             * @brief 在命令上声明标量公共选项（牌堆/人数/手牌/种子/日志/存档/AI 难度）。
             * @param cmd   目标命令：根命令或会读取这些选项的 leaf。
             * @param rules 玩家数上下限来源。
             * @note pjh_cli 的选项查找沿父链（名与值都取最近声明处），故 leaf 不重
             *       声明也能解析祖先的选项；此处 per-leaf 重声明只为让 leaf 的帮助/
             *       用法行列出这些选项。未显式给的项仍由 options_from 沿父链或会话
             *       启动选项回落，声明处一律不设默认值；标量「最近声明胜出」即期望
             *       语义，故 per-leaf 重声明无副作用。--ai 走 enum 映射，值域外
             *       输入在解析期报 enum_value_error（与未知选项同一 rc=2 错误面）。
             */
            inline void declare_common_options(
                pjh::cli::BaseCommand &cmd, const tkw::game::RulesConfig &rules)
            {
                cmd.option<fixed_string("deck")>(
                       "--deck", 'd', "资源目录（含 deck.json 与 cards/）")
                    .path();
                cmd.option<fixed_string("players")>("--players", 'p', "玩家数")
                    .integer()
                    .min(rules.min_players)
                    .max(rules.max_players);
                cmd.option<fixed_string("hand")>("--hand", "初始手牌数")
                    .integer()
                    .min(0)
                    .max(20);
                cmd.option<fixed_string("seed")>("--seed", 's', "随机种子")
                    .integer()
                    .min(0);
                cmd.option<fixed_string("verbose")>(
                       "--verbose", 'v', "打印卡牌/死亡事件日志")
                    .boolean();
                cmd.option<fixed_string("autosave")>(
                       "--autosave", "REPL 退出时自动存档路径（空串关闭）")
                    .path();
                cmd.option<fixed_string("ai")>(
                       "--ai", "AI 难度：simple 贪心 / aggressive 伤害优先")
                    .enum_type<AiLevel>()
                    .mapping({{"simple", AiLevel::Simple},
                             {"aggressive", AiLevel::Aggressive}});
            }

            /**
             * @brief 在根命令上声明可重复的真人座位选项。
             * @param cmd 目标命令；只应传根命令，使父/叶混写累积进同一上下文。
             * @note repeatable 选项的值按「最近声明」写入单一上下文，若根与 leaf 各
             *       声明一份，`--human P0 new --human P1` 会分落两处，而读取只取最近
             *       节点，导致 P0 静默丢弃；故仅根声明。leaf 处仍可解析（祖先链查找）。
             *       值补全候选取自规则允许的座位上限，防止越界座位号到运行期才报错。
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
            }

            /** 性别占位：无玩家数据源，按座位奇偶交替（P0 男 / P1 女 / …）。 */
            inline tkw::entity::Gender gender_for_seat(int seat)
            {
                return seat % 2 == 0 ? tkw::entity::Gender::Male
                                     : tkw::entity::Gender::Female;
            }

            /** 按选项构建一局（加载牌堆 + 建玩家）；失败返回 nullptr 并填 err。 */
            inline std::unique_ptr<tkw::game::Game> build_game(
                const Options &opt, std::string &err)
            {
                tkw::config::ResourceStore store(opt.deck);
                auto catalog = tkw::card::CardDefCatalog::load(store, "deck");
                if (catalog.is_err())
                {
                    const auto &e = catalog.unwrap_err();
                    err = "加载牌堆失败 (kind=" +
                          std::to_string(static_cast<int>(e.kind)) + "): " + e.detail;
                    return nullptr;
                }
                auto game = std::make_unique<tkw::game::Game>(
                    std::move(catalog).unwrap(),
                    std::make_unique<tkw::SeededRng>(opt.seed));
                for (int i = 0; i < opt.players; ++i)
                {
                    auto r = game->add_player(
                        "P" + std::to_string(i), i,
                        tkw::entity::Hp::make(game->rules.base_hp),
                        gender_for_seat(i));
                    if (r.is_err())
                    {
                        err = "创建玩家失败: P" + std::to_string(i);
                        return nullptr;
                    }
                }
                return game;
            }

            /** 卡牌 id → 目录中文名；目录未收录该 id 或名称为空时回落 id 本身。 */
            inline const std::string &card_name(
                const tkw::card::CardDefCatalog &catalog, const std::string &def_id)
            {
                const auto def = catalog.find(def_id);
                if (def.is_some() && !def.unwrap()->name.empty())
                    return def.unwrap()->name;
                return def_id;
            }

            /** 未实现卡展示名：目录中文名 + (id) 后缀；目录未收录时回落 id。 */
            inline std::string audit_entry_name(
                const tkw::card::CardDefCatalog &catalog, const std::string &def_id)
            {
                return card_name(catalog, def_id) + "(" + def_id + ")";
            }

            /** 卡牌大类 → 中文展示（基本/锦囊/装备）。 */
            inline constexpr const char *card_type_zh(const tkw::card::CardType t)
            {
                switch (t)
                {
                case tkw::card::CardType::Basic:
                    return "基本";
                case tkw::card::CardType::Trick:
                    return "锦囊";
                default:
                    return "装备";
                }
            }

            /**
             * @brief 订阅本局事件日志：verbose 为真时打印摸牌/打牌/弃牌/伤害/体力/阵亡。
             * @return 订阅句柄；verbose 为假时为空，句柄析构即退订。
             * @note 句柄只应活在需要日志的命令作用域内，不得存入 Session：会话被覆盖
             *       时会先析构旧 Game（含总线），遗留句柄将对已释放总线退订。
             */
            inline std::vector<tkw::EventBus::Handle> subscribe_event_log(
                tkw::game::Game &game, bool verbose)
            {
                std::vector<tkw::EventBus::Handle> handles;
                if (!verbose)
                    return handles;
                handles.push_back(game.bus.subscribe(tkw::Handler<tkw::CardPlayedEvent>(
                    [&catalog = game.catalog](tkw::HandlerContext<tkw::CardPlayedEvent> &c)
                    {
                        std::cout << "[打出] " << c.event.user << " "
                                  << card_name(catalog, c.event.def_id) << "\n";
                    })));
                handles.push_back(
                    game.bus.subscribe(tkw::Handler<tkw::CardDiscardedEvent>(
                        [&catalog = game.catalog](
                            tkw::HandlerContext<tkw::CardDiscardedEvent> &c) {
                            std::cout << "[弃置] " << c.event.entity << " "
                                      << card_name(catalog, c.event.def_id) << "\n";
                        })));
                handles.push_back(game.bus.subscribe(tkw::Handler<tkw::CardDrawnEvent>(
                    [&catalog = game.catalog](
                        tkw::HandlerContext<tkw::CardDrawnEvent> &c) {
                        std::cout << "[摸牌] " << c.event.entity << " "
                                  << card_name(catalog, c.event.def_id) << "\n";
                    })));
                handles.push_back(
                    game.bus.subscribe(tkw::Handler<tkw::EntityDamagedEvent>(
                        [](tkw::HandlerContext<tkw::EntityDamagedEvent> &c) {
                            // 无来源 = 闪电等非玩家来源，渲染为 (无来源) 避免空段。
                            std::string_view src = c.event.source;
                            if (src.empty())
                                src = "(无来源)";
                            std::cout << "[伤害] " << src << " -> "
                                      << c.event.target << " " << c.event.amount
                                      << "\n";
                        })));
                handles.push_back(
                    game.bus.subscribe(tkw::Handler<tkw::EntityHpChangedEvent>(
                        [](tkw::HandlerContext<tkw::EntityHpChangedEvent> &c) {
                            std::cout << "[体力] " << c.event.entity_id << " "
                                      << c.event.old_cur << "->" << c.event.new_cur
                                      << "/" << c.event.max << "\n";
                        })));
                handles.push_back(
                    game.bus.subscribe(tkw::Handler<tkw::EntityDiedEvent>(
                        [](tkw::HandlerContext<tkw::EntityDiedEvent> &c)
                        { std::cout << "[阵亡] " << c.event.entity_id << "\n"; })));
                return handles;
            }

            /**
             * @brief 订阅对局统计事件：把伤害/治疗/死亡累计入 stats（击杀按最近伤害来源归因）。
             * @param game  本局运行时；死亡事件处理中按存活实体判定击杀归属。
             * @param stats 聚合目标；句柄析构即退订，stats 须比句柄存活更久。
             * @return 订阅句柄集合。
             * @note 死亡事件不带击杀者字段，归因取「受害者最近一次伤害来源」；来源为空
             *       （闪电等）或已阵亡（同归于尽时先死者）不计击杀，与引擎击杀奖励口径一致。
             */
            inline std::vector<tkw::EventBus::Handle> subscribe_stats(
                tkw::game::Game &game, BattleStats &stats)
            {
                std::vector<tkw::EventBus::Handle> handles;
                handles.push_back(
                    game.bus.subscribe(tkw::Handler<tkw::EntityDamagedEvent>(
                        [&stats](tkw::HandlerContext<tkw::EntityDamagedEvent> &c)
                        {
                            stats.last_hit_source[c.event.target] = c.event.source;
                            if (c.event.source.empty())
                                return;
                            stats.damage_dealt[c.event.source] += c.event.amount;
                        })));
                handles.push_back(game.bus.subscribe(tkw::Handler<tkw::EntityHealedEvent>(
                    [&stats](tkw::HandlerContext<tkw::EntityHealedEvent> &c)
                    { stats.healing[c.event.target] += c.event.amount; })));
                handles.push_back(
                    game.bus.subscribe(tkw::Handler<tkw::EntityDiedEvent>(
                        [&game, &stats](tkw::HandlerContext<tkw::EntityDiedEvent> &c)
                        {
                            stats.died.insert(c.event.entity_id);
                            const auto hit = stats.last_hit_source.find(c.event.entity_id);
                            if (hit == stats.last_hit_source.end())
                                return;
                            if (game.entities.find(hit->second).is_some())
                                ++stats.kills[hit->second];
                        })));
                return handles;
            }

            /**
             * @brief 打印对局统计块（对局结束时调用，追加在胜者/平局行之后）。
             * @param stats  本局累计的统计聚合。
             * @param game   本局运行时；存活实体体力在此读取（阵亡者显示「阵亡」）。
             * @param winner 胜者 id；空串显示「无」（平局/同归于尽）。
             * @param turns  已执行回合数。
             * @note 玩家清单 = 存活实体 ∪ 阵亡记录，按 id 排序输出；统计以已发布事件
             *       为准，存档恢复的会话只含读档后的事件，此前部分不计入。
             */
            inline void print_battle_stats(
                const BattleStats &stats, const tkw::game::Game &game,
                const std::string &winner, int turns)
            {
                const auto val = [](const std::map<std::string, int> &m,
                                    const std::string &k)
                {
                    const auto it = m.find(k);
                    return it == m.end() ? 0 : it->second;
                };
                std::cout << "对局统计:\n";
                std::cout << "  回合数: " << turns << "\n";
                std::cout << "  胜者: " << (winner.empty() ? "无" : winner) << "\n";
                std::set<std::string> ids = stats.died;
                for (const auto &e : game.entities)
                    ids.insert(e->get_id());
                for (const auto &id : ids)
                {
                    const auto alive = game.entities.find(id);
                    std::string hp;
                    if (alive.is_some())
                        hp = "体力 " + std::to_string(alive.unwrap()->get_hp()) +
                             "/" + std::to_string(alive.unwrap()->get_hp_bar().get_max());
                    else
                        hp = "阵亡";
                    std::cout << "  " << id << ": " << hp << "，击杀 "
                              << val(stats.kills, id) << "，伤害 "
                              << val(stats.damage_dealt, id) << "，治疗 "
                              << val(stats.healing, id) << "\n";
                }
            }

            /** 校验真人座位：必须是对局中存在的实体且互不重复；空串表示通过。 */
            inline std::string validate_humans(
                tkw::game::Game &game, const std::vector<std::string> &humans)
            {
                auto ctx = game.context();
                for (std::size_t i = 0; i < humans.size(); ++i)
                {
                    if (ctx.entities->find(humans[i]).is_none())
                        return "真人座位不存在: " + humans[i];
                    for (std::size_t j = i + 1; j < humans.size(); ++j)
                        if (humans[i] == humans[j])
                            return "真人座位重复: " + humans[i];
                }
                return {};
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

            inline void print_status(const Session &s)
            {
                std::cout << "会话: " << (s.active ? "进行中" : "无") << "\n";
                if (!s.active || !s.game)
                    return;
                auto ctx = s.game->context();
                std::cout << "  下一回合: " << s.state.current
                          << "，已执行回合: " << s.state.turns
                          << "，存活: " << ctx.entities->size() << "\n";
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

            inline CliResult<void> cmd_new(const Options &opt, Session &s)
            {
                std::string err;
                auto game = build_game(opt, err);
                if (!game)
                    return CliFailure{CliError(err)};
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
                    return CliFailure{CliError("没有进行中的对局")};
                auto log = subscribe_event_log(*s.game, verbose);
                auto stats_handles = subscribe_stats(*s.game, s.stats);
                auto ai = make_decision_source(s.humans, s.ai);
                auto ctx = s.game->context();
                if (tkw::game::session_over(ctx))
                {
                    std::cout << "对局已结束，胜者: "
                              << tkw::game::session_winner(ctx) << "\n";
                    return CliResult<void>::Ok();
                }
                auto r = tkw::game::step_session(ctx, *ai, s.state);
                if (r.is_err())
                {
                    if (r.unwrap_err() == tkw::game::LoopError::MaxRounds)
                    {
                        std::cout << "平局（达到最大回合数）\n";
                        print_battle_stats(s.stats, *s.game, {}, s.state.turns);
                        return CliResult<void>::Ok();
                    }
                    return CliFailure{CliError("回合执行失败")};
                }
                print_status(s);
                if (tkw::game::session_over(ctx))
                {
                    std::cout << "对局结束，胜者: " << tkw::game::session_winner(ctx)
                              << "\n";
                    print_battle_stats(
                        s.stats, *s.game, tkw::game::session_winner(ctx), s.state.turns);
                }
                return CliResult<void>::Ok();
            }

            inline CliResult<void> cmd_run(Session &s, bool verbose)
            {
                if (!s.active || !s.game)
                    return CliFailure{CliError("没有进行中的对局")};
                auto log = subscribe_event_log(*s.game, verbose);
                auto stats_handles = subscribe_stats(*s.game, s.stats);
                auto ai = make_decision_source(s.humans, s.ai);
                auto ctx = s.game->context();
                bool advanced = false;
                while (!tkw::game::session_over(ctx))
                {
                    auto r = tkw::game::step_session(ctx, *ai, s.state);
                    if (r.is_err())
                    {
                        if (r.unwrap_err() == tkw::game::LoopError::MaxRounds)
                        {
                            std::cout << "平局（达到最大回合数）\n";
                            print_battle_stats(s.stats, *s.game, {}, s.state.turns);
                            return CliResult<void>::Ok();
                        }
                        return CliFailure{CliError("回合执行失败")};
                    }
                    advanced = true;
                }
                std::cout << "胜者: " << tkw::game::session_winner(ctx)
                          << "，回合数: " << s.state.turns << "\n";
                // 对局在本次命令内跑完才附统计块；已在更早 step 结束时不重复打印。
                if (advanced)
                    print_battle_stats(
                        s.stats, *s.game, tkw::game::session_winner(ctx), s.state.turns);
                return CliResult<void>::Ok();
            }

            inline CliResult<void> cmd_save(
                const std::filesystem::path &file, Session &s)
            {
                if (!s.active || !s.game)
                    return CliFailure{CliError("没有进行中的对局")};
                const std::string text = tkw::save::write(*s.game, s.state, "deck");
                if (tkw::io::write_text_atomic(file, text).is_err())
                    return CliFailure{CliError("写入存档失败: " + file.string())};
                std::cout << "已保存: " << file.string() << "\n";
                return CliResult<void>::Ok();
            }

            inline CliResult<void> cmd_load(
                const Options &opt, const std::filesystem::path &file, Session &s)
            {
                auto text = tkw::io::read_text(file);
                if (text.is_err())
                    return CliFailure{CliError("读取存档失败: " + file.string())};
                std::string err;
                auto game = build_game(opt, err);
                if (!game)
                    return CliFailure{CliError(err)};
                tkw::game::GameSession state;
                auto r = tkw::save::read(text.unwrap(), *game, state);
                if (r.is_err())
                {
                    const auto &e = r.unwrap_err();
                    return CliFailure{CliError(
                        "存档加载失败 (kind=" +
                        std::to_string(static_cast<int>(e.kind)) + "): " + e.detail)};
                }
                const std::string verr = validate_humans(*game, opt.humans);
                if (!verr.empty())
                    return CliFailure{CliError(verr)};
                s.game = std::move(game);
                s.state = std::move(state);
                s.humans = opt.humans;
                s.ai = opt.ai;
                s.verbose = opt.verbose;
                s.active = true;
                s.stats = BattleStats{};
                std::cout << "已加载: " << file.string() << "\n";
                print_status(s);
                return CliResult<void>::Ok();
            }

            inline CliResult<void> run_game(const Options &opt)
            {
                std::string err;
                auto game = build_game(opt, err);
                if (!game)
                    return CliFailure{CliError(err)};

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
                while (!tkw::game::session_over(ctx))
                {
                    auto r = tkw::game::step_session(ctx, *ai, session);
                    if (r.is_err())
                    {
                        const auto code = r.unwrap_err();
                        if (code == tkw::game::LoopError::MaxRounds)
                        {
                            std::cout << "平局（达到最大回合数）\n";
                            print_battle_stats(stats, *game, {}, session.turns);
                            return CliResult<void>::Ok();
                        }
                        return CliFailure{CliError(
                            "对局失败 (LoopError=" +
                            std::to_string(static_cast<int>(code)) + ")")};
                    }
                }
                const std::string winner = tkw::game::session_winner(ctx);
                std::cout << "胜者: " << winner
                          << "，回合数: " << session.turns << "\n";
                print_battle_stats(stats, *game, winner, session.turns);
                return CliResult<void>::Ok();
            }

            inline CliResult<void> audit_deck(const Options &opt)
            {
                std::string err;
                auto game = build_game(opt, err);
                if (!game)
                    return CliFailure{CliError(err)};

                const auto unsupported =
                    tkw::game::unsupported_cards(game->catalog);
                if (unsupported.empty())
                {
                    std::cout << "牌堆全部可结算\n";
                    return CliResult<void>::Ok();
                }
                std::cout << "未实现卡（" << unsupported.size() << " 张）:\n";
                for (const auto &id : unsupported)
                    std::cout << "  " << audit_entry_name(game->catalog, id)
                              << "\n";
                return CliResult<void>::Ok();
            }

            /**
             * @brief 列出牌表：只加载卡牌目录，打印牌堆名与种类/张数，再按
             *        deck 序逐卡打印「中文名(id) 大类 张数」。
             * @return Ok；Err 为牌堆加载失败（kind + detail，与建局错误面一致）。
             * @note 只读牌堆查询，不建局、不消耗随机源；公共选项中仅 --deck
             *       生效，其余被接受但不读取（与 audit 声明面一致）。deck.json
             *       的 name 缺失或类型不符时头行退化为无牌堆名，不阻断列出。
             */
            inline CliResult<void> cards_list(const Options &opt)
            {
                tkw::config::ResourceStore store(opt.deck);
                auto catalog = tkw::card::CardDefCatalog::load(store, "deck");
                if (catalog.is_err())
                {
                    const auto &e = catalog.unwrap_err();
                    return CliFailure{CliError(
                        "加载牌堆失败 (kind=" +
                        std::to_string(static_cast<int>(e.kind)) + "): " + e.detail)};
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

            /** 把 "Usage: " 前缀换成中文，其余原样（帮助与 REPL 无匹配提示共用）。 */
            inline std::string zh_usage_prefix(std::string text)
            {
                constexpr std::string_view prefix = "Usage: ";
                if (text.starts_with(prefix))
                    text.replace(0, prefix.size(), "用法: ");
                return text;
            }

            /** 把帮助正文中独占一行的英文段标题替换为中文（只替换首个匹配）。 */
            inline void replace_heading_line(
                std::string &text, std::string_view from, std::string_view to)
            {
                const std::string needle = "\n" + std::string(from) + ":\n";
                const std::string replacement = "\n" + std::string(to) + ":\n";
                if (std::size_t pos = text.find(needle); pos != std::string::npos)
                    text.replace(pos, needle.size(), replacement);
            }

            /**
             * @brief 按命令名列表渲染「命令名（含别名）+ 描述」两列。
             * @param root  根命令，用于按名查子命令的别名与描述。
             * @param names 命令名列表（查询结果为规范名，别名从命令树取）。
             * @return 两列文本；无别名命令与既有输出逐字一致，描述缺失留空。
             * @note 列宽按带别名后缀的名字计算，保证行对齐。
             */
            inline std::string command_lines_zh(
                const pjh::cli::BranchCommand &root,
                const std::vector<std::string> &names)
            {
                std::vector<std::string> lefts;
                std::vector<std::string> descs;
                lefts.reserve(names.size());
                descs.reserve(names.size());

                for (const auto &n : names)
                {
                    std::string left = n;
                    std::string desc;
                    const pjh::cli::BaseCommand *sub = root.find_subcommand(n);
                    if (sub != nullptr)
                    {
                        desc = sub->description();
                        const auto &aliases = sub->aliases();
                        if (!aliases.empty())
                        {
                            left += " (";
                            for (std::size_t i = 0; i < aliases.size(); ++i)
                            {
                                if (i > 0)
                                    left += ", ";
                                left += aliases[i];
                            }
                            left += ")";
                        }
                    }
                    lefts.push_back(std::move(left));
                    descs.push_back(std::move(desc));
                }

                std::size_t width = 0;
                for (const auto &l : lefts)
                    if (l.size() > width)
                        width = l.size();

                std::string out;
                for (std::size_t i = 0; i < names.size(); ++i)
                {
                    out += "  " + lefts[i];
                    out.append(width - lefts[i].size(), ' ');
                    out += "  ";
                    out += descs[i];
                    out += "\n";
                }
                return out;
            }
        }  // namespace detail

        /**
         * @brief 把命令树的框架帮助数据渲染为中文帮助。
         * @param cmd 请求帮助的命令（根或任一子命令）。
         * @return 中文段标题（用法/选项/公共选项/参数/子命令）的完整帮助；根命令额外附用法示例。
         * @note 只替换框架渲染结果的段标题与 usage 前缀，选项/参数/子命令的排布与
         *       对齐仍由框架负责，避免自造排版。段标题在渲染后再替换，使框架仍按
         *       英文段名选择列宽上限（选项段 32 字节）。选项标注（如 (repeatable)）
         *       保持框架原文。program_name 用完整命令路径，子命令帮助也带 tkw 前缀。
         *       根帮助示例按「批量一次性」与「REPL 会话」分组：会话流命令只能在
         *       `tkw repl` 内逐条输入，不带 tkw 前缀。
         */
        inline std::string render_help_zh(const pjh::cli::BaseCommand &cmd)
        {
            std::vector<std::string_view> parts;
            for (const pjh::cli::BaseCommand *c = &cmd; c != nullptr; c = c->parent())
                if (!c->name().empty())
                    parts.push_back(c->name());
            std::string path;
            for (auto it = parts.rbegin(); it != parts.rend(); ++it)
            {
                if (!path.empty())
                    path += ' ';
                path += *it;
            }

            pjh::cli::HelpInfo info = pjh::cli::HelpFormatter::collect_help(cmd, path);
            pjh::cli::HelpDocument doc = pjh::cli::HelpFormatter::build_document(info);

            std::string text =
                detail::zh_usage_prefix(pjh::cli::HelpFormatter::format_help(doc));
            detail::replace_heading_line(text, "Options", "选项");
            detail::replace_heading_line(text, "Inherited Options", "公共选项");
            detail::replace_heading_line(text, "Arguments", "参数");
            detail::replace_heading_line(text, "Subcommands", "子命令");

            if (cmd.parent() == nullptr)
                text +=
                    "示例:\n"
                    "  批量一次性:\n"
                    "    tkw                      跑一局 AI 对局\n"
                    "    tkw deal 2 1             按位置参数跑一局（2 人，种子 1）\n"
                    "    tkw --ai aggressive deal 2 1  aggressive AI 跑一局\n"
                    "    tkw audit                审计牌堆\n"
                    "    tkw cards              列出牌表构成\n"
                    "  REPL 会话（先 tkw repl，再逐条输入）:\n"
                    "    new --players 2 --seed 1 开新局\n"
                    "    step                     执行一个回合\n"
                    "    status                   查看会话状态\n"
                    "    save s.json              保存当前对局\n"
                    "    load s.json              载入存档到会话，再 step 继续\n"
                    "  真人参与（先 tkw --human P0 repl，再逐条输入）:\n"
                    "    new --players 2 --seed 1 开新局\n"
                    "    step                     轮到 P0 时按提示输入（play/pass/discard）\n";
            return text;
        }

        /**
         * @brief REPL `?` 查询结果的中文渲染。
         * @param root   根命令，用于按名查子命令描述。
         * @param result 框架查询结果（列表/匹配/模糊/无匹配）。
         * @return 中文提示 + 命令名与描述两列；无匹配时附中文用法行。
         * @note 只消费框架结构，不重复实现匹配逻辑；描述直接取命令树，保证与
         *       --help 子命令表同源。
         */
        inline std::string render_query_zh(
            const pjh::cli::BranchCommand &root,
            const pjh::cli::QueryResult &result)
        {
            using pjh::cli::QueryKind;
            switch (result.kind)
            {
            case QueryKind::Listing:
                return "命令（? <关键词> 过滤，help <命令> 看用法）:\n" +
                       detail::command_lines_zh(root, result.names);
            case QueryKind::Matched:
                return "匹配命令:\n" + detail::command_lines_zh(root, result.names);
            case QueryKind::Fuzzy:
            {
                std::string out = "您是否要找:";
                for (const auto &m : result.suggestions.matches)
                    out += " " + m.name;
                return out + "\n";
            }
            case QueryKind::NoMatch:
                // 调用方统一在结果末尾补一个换行，这里不自带换行以免多出空行。
                return "没有匹配的命令。" + detail::zh_usage_prefix(result.usage_line);
            }
            return {};
        }

        /**
         * @brief REPL `help [命令]` 的中文渲染。
         * @param result 框架导航结果。
         * @return 根/子命令帮助走同一中文渲染；叶命令与未知命令用中文提示。
         * @note 帮助正文复用 render_help_zh，保证 REPL `help` 与批量 `--help` 同格式。
         */
        inline std::string render_help_nav_zh(
            const pjh::cli::HelpNavigationResult &result)
        {
            using pjh::cli::HelpNavigationKind;
            switch (result.kind)
            {
            case HelpNavigationKind::RootHelp:
            case HelpNavigationKind::SubcommandHelp:
                return render_help_zh(*result.resolved);
            case HelpNavigationKind::NonBranch:
                return "'" + result.failed_command_name + "' 没有子命令。\n";
            case HelpNavigationKind::UnknownCommand:
            {
                std::string out = "未知命令 '" + result.failed_token + "'。";
                if (!result.suggestions.matches.empty())
                {
                    out += " 您是否要找:";
                    for (const auto &m : result.suggestions.matches)
                        out += " " + m.name;
                }
                return out + "\n";
            }
            }
            return {};
        }

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

            // 根命令选项（无子命令时直接跑一局，兼容旧用法）；读取这些选项的 leaf
            // 各自声明一份标量选项，使子命令名之后的选项也可解析；--human 是
            // repeatable，仅根声明以避免父/叶混写时值分落两处。
            detail::declare_common_options(app, rules);
            detail::declare_human_option(app);

            // 根命令：无子命令直接跑一局（兼容旧用法），先打印一行引导。
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
                        return CliFailure{CliError("玩家数超出允许范围")};
                    return detail::run_game(opt);
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
            status_cmd.action(
                [&session](ParseContext &) -> CliResult<void>
                {
                    detail::print_status(session);
                    return CliResult<void>::Ok();
                });

            // save：保存当前对局（别名 w）
            auto &save_cmd = app.add_leaf("save", "保存当前对局：save <file>");
            save_cmd.alias("w");
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
                        ctx.get<std::string, 0>(), session);
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
                    // 进入 REPL 前打印引导：命令列表与退出方式在提示符处不可见。
                    std::cout << "输入 ? 查看命令，help <命令> 看用法，quit 退出\n";
                    InteractiveConsole console(
                        app, "tkw> ", std::cin, std::cout, std::cerr,
                        [&app](const pjh::cli::QueryResult &r)
                        { return render_query_zh(app, r); },
                        [](const pjh::cli::HelpNavigationResult &r)
                        { return render_help_nav_zh(r); });
                    console.run();
                    const Options &opt = session.base;
                    if (session.active && session.game && !opt.autosave.empty())
                    {
                        const std::string text =
                            tkw::save::write(*session.game, session.state, "deck");
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
