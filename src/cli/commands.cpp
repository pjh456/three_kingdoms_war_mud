/**
 * @file   commands.cpp
 * @brief  CLI 命令树与命令执行体的定义。
 * @ingroup tkw_cli
 */

#include "cli/commands.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <pjh_cli.hpp>

namespace tkw
{
    namespace cli
    {
        namespace detail
        {
            Options options_from(ParseContext &ctx, const Options &base)
            {
                Options opt = base;
                opt.deck =
                    ctx.get_or<std::filesystem::path, fixed_string("deck")>(base.deck);
                opt.players = ctx.get_or<int, fixed_string("players")>(base.players);
                opt.hand = ctx.get_or<int, fixed_string("hand")>(base.hand);
                opt.seed = static_cast<std::uint32_t>(
                    ctx.get_or<int, fixed_string("seed")>(static_cast<int>(base.seed)));
                // 显式提供（含 --no-verbose）才覆盖会话继承；未提供时保持启动/会话值。
                // verbose_explicit 随继承传递，使启动 --no-verbose 能压过真人局默认日志。
                const bool verbose_provided =
                    ctx.was_provided<fixed_string("verbose")>();
                opt.verbose = verbose_provided
                                  ? ctx.get<bool, fixed_string("verbose")>()
                                  : base.verbose;
                opt.verbose_explicit = verbose_provided || base.verbose_explicit;
                opt.ai = ctx.get_or_enum<AiLevel, fixed_string("ai")>(base.ai);
                opt.mode =
                    ctx.get_or_enum<tkw::game::GameMode, fixed_string("mode")>(
                        base.mode);
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

                // 武将选择同 --human：可重复累积，未提供时保持继承值（无 --no-hero 清空）
                if (ctx.has<fixed_string("hero")>())
                    opt.heroes = ctx.get_all<std::string, fixed_string("hero")>();
                return opt;
            }

            Options options_from(ParseContext &ctx)
            {
                return options_from(ctx, Options{});
            }

            Options options_from_for_query(
                ParseContext &ctx, const Session &session)
            {
                Options seed = session.base;
                // 活动会话存在时以会话牌表为默认上下文，使牌表来源可追溯到用户最近一次建局。
                if (session.active && session.game)
                    seed.deck = session.deck;

                seed.humans.clear();
                return options_from(ctx, seed);
            }

            bool resolve_verbose(ParseContext &ctx, const Session &session)
            {
                return ctx.was_provided<fixed_string("verbose")>()
                           ? ctx.get<bool, fixed_string("verbose")>()
                           : session.verbose;
            }

            bool session_verbose(const Options &opt)
            {
                if (opt.verbose_explicit)
                    return opt.verbose;
                return opt.verbose || !opt.humans.empty();
            }

            std::unique_ptr<pjh::cli::IHistory> make_repl_history(
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

            std::vector<std::pair<std::string, AiLevel>> ai_level_mappings()
            {
                std::vector<std::pair<std::string, AiLevel>> mappings;
                for (const auto &entry : kAiLevelTexts)
                    mappings.emplace_back(entry.name, entry.level);
                return mappings;
            }

            void declare_common_options(
                pjh::cli::BaseCommand &cmd, const tkw::game::RulesConfig &rules)
            {
                cmd.option<fixed_string("deck")>(
                       "--deck", 'd',
                       "资源目录（含 deck.json 与 cards/，默认 resources）")
                    .path();
                cmd.option<fixed_string("players")>(
                       "--players", 'p',
                       "玩家数（2–8；REPL 内缺省继承启动 --players，否则默认 4）")
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
                       "打印事件日志（摸牌/击杀奖励/打出/弃置/判定/移牌/伤害/体力/阵亡；真人局默认开启，--no-verbose 关闭；对手摸牌显示「未知牌」）")
                    .boolean()
                    .negatable();
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
                    .mapping(ai_level_mappings())
                    .completer([] {
                        std::vector<std::string> names;
                        for (const auto &m : ai_level_mappings())
                            names.push_back(m.first);
                        return names;
                    });
                // CLI 输入文本契约：`--mode` 的值域由下方 mapping/completer
                // 定义。与存档文本（`save::mode_name`/`mode_from`）分属独立契约，
                // 不得合并。新增模式须同步本 mapping/completer、
                // `src/tui/command.cpp` 的 `mode_from` 与 `save/format.{hpp,cpp}`。
                cmd.option<fixed_string("mode")>(
                       "--mode",
                       "对局模式：brawl 乱斗 / identity 身份局（默认 brawl；identity 需 4–8 人）")
                    .enum_type<tkw::game::GameMode>()
                    .mapping({{"brawl", tkw::game::GameMode::Brawl},
                              {"identity", tkw::game::GameMode::Identity}})
                    .completer([] {
                        // 候选值域与上方 enum 映射保持一致
                        return std::vector<std::string>{"brawl", "identity"};
                    });
            }

            void declare_human_option(pjh::cli::BaseCommand &cmd)
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

            void declare_hero_option(pjh::cli::BaseCommand &cmd)
            {
                cmd.option<fixed_string("hero")>(
                       "--hero",
                       "武将选择（可重复：--hero P0=zhangfei；武将随牌表目录的 heroes.json）")
                    .str()
                    .repeatable()
                    .completer([] {
                        std::vector<std::string> seats;
                        const int max_players = tkw::game::RulesConfig{}.max_players;
                        for (int i = 0; i < max_players; ++i)
                            seats.push_back("P" + std::to_string(i) + "=");
                        return seats;
                    });
            }

            tkw::game::BuildOptions build_options_from(
                const Options &opt, tkw::game::GameMode mode)
            {
                return tkw::game::BuildOptions{opt.deck, opt.players, opt.seed, mode};
            }

            tkw::game::BuildOptions build_options_from(const Options &opt)
            {
                return build_options_from(opt, opt.mode);
            }

            tkw::Result<HeroAssignments, std::string>
            parse_hero_assignments(
                const std::vector<std::string> &raw, int players)
            {
                HeroAssignments out;
                std::vector<std::string> all_seats;
                all_seats.reserve(static_cast<std::size_t>(players));
                for (int i = 0; i < players; ++i)
                    all_seats.push_back("P" + std::to_string(i));
                const std::string seat_hint =
                    all_seats.empty()
                        ? "（无可用座位）"
                        : "（可用座位: " + join_items(all_seats, "、") + "）";
                for (const auto &item : raw)
                {
                    const auto eq = item.find('=');
                    if (eq == std::string::npos || eq == 0 ||
                        eq + 1 >= item.size())
                        return tkw::Result<HeroAssignments,
                                           std::string>::Err(
                            "武将选项格式须为 座位=武将（如 P0=zhangfei）: " + item);

                    const std::string seat = item.substr(0, eq);
                    const std::string hero_id = item.substr(eq + 1);
                    if (seat.size() < 2 || seat.front() != 'P')
                        return tkw::Result<HeroAssignments,
                                           std::string>::Err(
                            "武将座位须形如 P0: " + item);

                    int index = 0;
                    const char *begin = seat.data() + 1;
                    const char *end = seat.data() + seat.size();
                    const auto r = std::from_chars(begin, end, index);
                    if (r.ec != std::errc{} || r.ptr != end)
                        return tkw::Result<HeroAssignments,
                                           std::string>::Err(
                            "武将座位须形如 P0: " + item);
                    if (index < 0 || index >= players)
                        return tkw::Result<HeroAssignments,
                                           std::string>::Err(
                            "武将座位超出玩家数: " + seat + "（当前 " +
                            std::to_string(players) + " 人）" + seat_hint);

                    const std::string canonical = "P" + std::to_string(index);
                    if (!out.by_seat.emplace(canonical, hero_id).second)
                        return tkw::Result<HeroAssignments,
                                           std::string>::Err(
                            "武将座位重复: " + canonical +
                            "（每个座位只能指定一次）");
                }
                return tkw::Result<HeroAssignments,
                                   std::string>::Ok(std::move(out));
            }

            tkw::Result<tkw::game::BuildOptions, std::string>
            build_options_with_heroes(
                const Options &opt, tkw::game::GameMode mode)
            {
                auto parsed = parse_hero_assignments(opt.heroes, opt.players);
                if (parsed.is_err())
                    return tkw::Result<tkw::game::BuildOptions,
                                       std::string>::Err(parsed.unwrap_err());
                auto bo = build_options_from(opt, mode);
                bo.heroes = std::move(parsed).unwrap().by_seat;
                return tkw::Result<tkw::game::BuildOptions, std::string>::Ok(
                    std::move(bo));
            }

            tkw::Result<tkw::game::BuildOptions, std::string>
            build_options_with_heroes(const Options &opt)
            {
                return build_options_with_heroes(opt, opt.mode);
            }

            std::string unsupported_cards_warning_text(
                const tkw::card::CardDefCatalog &catalog,
                const std::vector<std::string> &unsupported)
            {
                if (unsupported.empty())
                    return {};
                std::string text = "警告: 牌堆含 " +
                                   std::to_string(unsupported.size()) +
                                   " 张引擎未实现的卡:";
                for (const auto &id : unsupported)
                    text += " " + audit_entry_name(catalog, id);
                return text;
            }

            std::vector<std::string> unsupported_cards_warning_lines(
                const tkw::card::CardDefCatalog &catalog)
            {
                const std::string text = unsupported_cards_warning_text(
                    catalog, tkw::game::unsupported_cards(catalog));
                return text.empty() ? std::vector<std::string>{}
                                    : std::vector<std::string>{text};
            }

            void warn_unsupported_cards(
                const tkw::card::CardDefCatalog &catalog,
                std::ostream &err)
            {
                for (const auto &line : unsupported_cards_warning_lines(catalog))
                    err << line << "\n";
            }

            std::string unsupported_hero_skill_warning_text(
                const tkw::hero::HeroDef &def)
            {
                std::string names;
                for (const auto skill : def.skills)
                {
                    if (!tkw::game::is_unimplemented_skill(skill))
                        continue;
                    if (!names.empty())
                        names += "、";
                    names += tkw::hero::display_skill_name(skill);
                }
                if (names.empty())
                    return {};
                return "警告: 武将 " + tkw::hero::display_hero_name(def) +
                       " 含引擎未实现的技能: " + names;
            }

            std::vector<std::string> unsupported_hero_skills_warning_lines(
                const tkw::game::Game &game)
            {
                std::vector<std::string> lines;
                for (const auto *e : game.entities.const_view())
                {
                    const std::string &hero_id = e->get_hero();
                    if (hero_id.empty())
                        continue;
                    const auto def = game.hero_catalog.find(hero_id);
                    if (def.is_none())
                        continue;
                    const std::string line =
                        unsupported_hero_skill_warning_text(*def.unwrap());
                    if (!line.empty())
                        lines.push_back(line);
                }
                return lines;
            }

            std::vector<std::string> unsupported_hero_skills_warning_lines(
                const tkw::hero::HeroCatalog &catalog,
                const std::map<std::string, std::string> &heroes)
            {
                std::set<std::string> seen;
                std::vector<std::string> lines;
                for (const auto &[seat, hero_id] : heroes)
                {
                    (void)seat;
                    if (hero_id.empty() || !seen.insert(hero_id).second)
                        continue;
                    const auto def = catalog.find(hero_id);
                    if (def.is_none())
                        continue;
                    const std::string line =
                        unsupported_hero_skill_warning_text(*def.unwrap());
                    if (!line.empty())
                        lines.push_back(line);
                }
                return lines;
            }

            void warn_unsupported_hero_skills(
                const tkw::game::Game &game, std::ostream &err)
            {
                for (const auto &line : unsupported_hero_skills_warning_lines(game))
                    err << line << "\n";
            }

            std::string player_range_error(
                int value, const tkw::game::RulesConfig &rules)
            {
                return "玩家数 " + std::to_string(value) + " 超出范围 [" +
                       std::to_string(rules.min_players) + ", " +
                       std::to_string(rules.max_players) + "]";
            }

            const char *no_active_game_error()
            {
                return "没有进行中的对局（先运行 new 开局，或进入 tkw repl）";
            }

            std::string validate_humans(
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

            std::string reject_humans(
                const std::vector<std::string> &humans, const std::string &cmd)
            {
                if (humans.empty())
                    return {};
                return cmd +
                       " 不支持 --human（该命令不运行真人参与的对局）；请直接运行 tkw " +
                       cmd;
            }

            std::unique_ptr<tkw::game::DecisionSource> make_decision_source(
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

            std::string turn_header_text(
                const tkw::game::GameSession &session)
            {
                return "—— 回合 " + std::to_string(session.turns + 1) + "：" +
                       session.current + " ——";
            }

            void print_turn_header(const tkw::game::GameSession &session)
            {
                std::cout << turn_header_text(session) << "\n";
            }

            tkw::game::LoopResult<RunOutcome> run_to_completion(
                tkw::game::GameContext &ctx, tkw::game::DecisionSource &ai,
                tkw::game::GameSession &session,
                tkw::game::TurnError *root,
                bool show_turn_headers,
                std::string *failed_actor)
            {
                while (!tkw::game::SessionQuery::session_over(ctx))
                {
                    if (show_turn_headers)
                        print_turn_header(session);
                    if (failed_actor)
                        *failed_actor = session.current;
                    auto r = tkw::game::GameLoop(ctx, ai).step_session(session, root);
                    if (r.is_ok())
                        continue;
                    if (r.unwrap_err() == tkw::game::LoopError::MaxRounds)
                        return tkw::game::LoopResult<RunOutcome>::Ok(
                            RunOutcome::MaxRounds);
                    return tkw::game::LoopResult<RunOutcome>::Err(r.unwrap_err());
                }
                return tkw::game::LoopResult<RunOutcome>::Ok(RunOutcome::Finished);
            }

            std::string game_end_label(const tkw::game::GameContext &ctx)
            {
                if (tkw::game::mode_of(ctx) == tkw::game::GameMode::Brawl)
                    return winner_label(tkw::game::SessionQuery::session_winner(ctx));
                return identity_result_label(
                    tkw::game::SessionQuery::session_camp(ctx),
                    tkw::game::SessionQuery::session_winner(ctx));
            }

            std::string game_stats_label(const tkw::game::GameContext &ctx)
            {
                if (tkw::game::mode_of(ctx) == tkw::game::GameMode::Brawl)
                    return tkw::game::SessionQuery::session_winner(ctx);
                return identity_result_label(
                    tkw::game::SessionQuery::session_camp(ctx),
                    tkw::game::SessionQuery::session_winner(ctx));
            }

            std::string zone_names(
                const tkw::game::ReadOnlyContext &ctx,
                const std::vector<tkw::card::Card> &zone)
            {
                if (zone.empty())
                    return "无";

                std::string out;
                for (std::size_t i = 0; i < zone.size(); ++i)
                {
                    if (i > 0)
                        out += "/";
                    out += tkw::card::display_name(ctx.catalog, zone[i].def_id);
                }
                return out;
            }

            void print_status(const Session &s)
            {
                if (!s.active || !s.game)
                {
                    std::cout << "会话: 无\n";
                    return;
                }

                auto ctx = s.game->context();
                const bool over = tkw::game::SessionQuery::session_over(ctx);
                // 达上限仅供展示提示：session_over 仍是唯一结束口径，会话未终结、
                // step 的致死击落仍可能产出唯一胜者，不能据此标为已结束。
                const bool at_cap =
                    !over && s.state.turns > tkw::game::rules_of(ctx).max_turns;
                std::cout << "会话: " << (over ? "已结束" : "进行中") << "\n";

                if (tkw::game::mode_of(ctx) == tkw::game::GameMode::Identity)
                    std::cout << "  模式: 身份局\n";

                // 已结束不再提示下一回合，与 run/deal 共用 game_end_label。
                if (over)
                    std::cout << "  胜者: " << game_end_label(ctx)
                              << "，已执行回合: " << s.state.turns
                              << "，存活: " << ctx.entities->size() << "\n";
                else
                {
                    std::cout << "  下一回合: " << s.state.current
                              << "，已执行回合: " << s.state.turns
                              << "，存活: " << ctx.entities->size() << "\n";
                    if (at_cap)
                        std::cout << "  提示: 已达回合上限；继续 step 仍有机会分出"
                                     "胜负，否则为平局\n";
                }

                std::cout << "  AI 难度: " << ai_level_name(s.ai) << "\n";
                std::cout << "  牌表: " << s.deck.string() << "\n";
                std::cout << "  真人座位: ";
                if (s.humans.empty())
                    std::cout << "无";
                else
                    for (std::size_t i = 0; i < s.humans.size(); ++i)
                        std::cout << (i == 0 ? "" : ",") << s.humans[i];
                std::cout << "\n";
                std::cout << "  局面:\n";
                const std::set<std::string> human_seats(s.humans.begin(),
                                                        s.humans.end());
                // 真人参与且未终局时收敛身份展示：仅主公与真人座位可见，其余以
                // 「未知」占位；全 AI 对局与终局一律公开，保持既有输出与身份局
                // 终局亮身份的惯例。
                const bool reveal_all_roles = s.humans.empty() || over;
                // 仅当任一实体有武将时才增加武将列，无 --hero 的默认输出不变
                bool any_hero = false;
                for (const auto *e : ctx.entities->const_view())
                {
                    if (!e->get_hero().empty())
                    {
                        any_hero = true;
                        break;
                    }
                }
                for (const auto &e : *ctx.entities)
                {
                    const std::string &id = e->get_id();
                    std::cout << "    " << id << " 体力 " << e->get_hp() << "/"
                              << e->get_hp_bar().get_max() << " 手牌 ";
                    if (human_seats.count(id) != 0)
                        std::cout << zone_names(
                            ctx, tkw::game::ai::make_view(ctx, id).hand);
                    else
                        std::cout << ctx.cards->hand_size(id);
                    std::cout << " 装备 " << zone_names(ctx, ctx.cards->equip(id))
                              << " 判定 " << zone_names(ctx, ctx.cards->judge(id));
                    if (tkw::game::mode_of(ctx) == tkw::game::GameMode::Identity)
                    {
                        const tkw::game::Role role =
                            tkw::game::role_of(ctx, e->get_id());
                        const bool visible =
                            reveal_all_roles ||
                            role == tkw::game::Role::Lord ||
                            human_seats.count(id) != 0;
                        std::cout << " 角色 "
                                  << role_label_zh(
                                         visible ? role : tkw::game::Role::None);
                    }
                    if (any_hero)
                        std::cout << " 武将 "
                                  << (e->get_hero().empty()
                                          ? std::string("无")
                                          : tkw::hero::display_hero_name(
                                                ctx.heroes, e->get_hero()));
                    if (e->get_chained())
                        std::cout << " " << kChainedTag;
                    std::cout << "\n";
                }
            }

            void print_max_rounds_draw(
                const tkw::save::BattleStats &stats, const tkw::game::Game &game,
                int turns)
            {
                std::cout << "平局（达到最大回合数）\n";
                print_battle_stats(stats, game, {}, turns);
            }

            CliResult<void> cmd_new(const Options &opt, Session &s)
            {
                auto options = build_options_with_heroes(opt);
                if (options.is_err())
                    return CliFailure{CliError(options.unwrap_err())};
                auto built = tkw::game::GameFactory::build(options.unwrap());
                if (built.is_err())
                    return CliFailure{CliError(format_build_error(built.unwrap_err()))};
                auto game = std::move(built).unwrap();
                warn_unsupported_cards(game->catalog);
                warn_unsupported_hero_skills(*game);
                const std::string verr = validate_humans(*game, opt.humans);
                if (!verr.empty())
                    return CliFailure{CliError(verr)};

                // 建局与后续 step/run 同口径：真人座位存在且未显式选 verbose 时开初始发牌日志。
                const bool verbose = session_verbose(opt);

                auto log = subscribe_event_log(*game, verbose, opt.humans);
                auto ctx = game->context();
                tkw::game::GameSession state;
                if (tkw::game::GameSetup(ctx)
                        .start_session(state, "P0", opt.hand)
                        .is_err())
                    return CliFailure{CliError("开局失败")};
                s.game = std::move(game);
                s.state = std::move(state);
                s.humans = opt.humans;
                s.ai = opt.ai;
                s.verbose = verbose;
                s.active = true;
                s.deck = opt.deck;
                s.stats = BattleStats{};
                std::cout << "新对局已开始\n";
                print_status(s);
                return CliResult<void>::Ok();
            }

            CliResult<void> cmd_step(Session &s, bool verbose)
            {
                if (!s.active || !s.game)
                    return CliFailure{CliError(no_active_game_error())};
                auto log = subscribe_event_log(*s.game, verbose, s.humans);
                auto stats_handles = subscribe_stats(*s.game, s.stats);
                auto ai = make_decision_source(s.humans, s.ai);
                auto ctx = s.game->context();
                if (tkw::game::SessionQuery::session_over(ctx))
                {
                    std::cout << "对局已结束，胜者: " << game_end_label(ctx) << "\n";
                    return CliResult<void>::Ok();
                }
                if (verbose)
                    print_turn_header(s.state);
                tkw::game::TurnError root = tkw::game::TurnError::PlayRejected;
                const std::string actor = s.state.current;  // 失败会推进，须先捕获
                auto r = tkw::game::GameLoop(ctx, *ai).step_session(s.state, &root);
                if (r.is_err())
                {
                    if (r.unwrap_err() == tkw::game::LoopError::MaxRounds)
                    {
                        print_max_rounds_draw(s.stats, *s.game, s.state.turns);
                        return CliResult<void>::Ok();
                    }
                    return CliFailure{CliError(
                        format_turn_failure(r.unwrap_err(), root, actor))};
                }
                if (tkw::game::SessionQuery::session_over(ctx))
                {
                    std::cout << "对局结束，胜者: " << game_end_label(ctx) << "\n";
                    print_battle_stats(
                        s.stats, *s.game, game_stats_label(ctx), s.state.turns);
                }
                else
                    print_status(s);
                return CliResult<void>::Ok();
            }

            CliResult<void> cmd_run(Session &s, bool verbose)
            {
                if (!s.active || !s.game)
                    return CliFailure{CliError(no_active_game_error())};
                auto log = subscribe_event_log(*s.game, verbose, s.humans);
                auto stats_handles = subscribe_stats(*s.game, s.stats);
                auto ai = make_decision_source(s.humans, s.ai);
                auto ctx = s.game->context();
                // 进入循环前判定：true 表示本次命令至少会推进（用于末尾统计门控）。
                const bool advanced = !tkw::game::SessionQuery::session_over(ctx);
                tkw::game::TurnError root = tkw::game::TurnError::PlayRejected;
                std::string actor = s.state.current;  // 出参每轮更新为失败角色
                auto rr =
                    run_to_completion(ctx, *ai, s.state, &root, verbose, &actor);
                if (rr.is_err())
                    return CliFailure{CliError(
                        format_turn_failure(rr.unwrap_err(), root, actor))};
                if (rr.unwrap() == RunOutcome::MaxRounds)
                {
                    print_max_rounds_draw(s.stats, *s.game, s.state.turns);
                    return CliResult<void>::Ok();
                }
                std::cout << "胜者: " << game_end_label(ctx)
                          << "，回合数: " << s.state.turns << "\n";
                // 对局在本次命令内跑完才附统计块；已在更早 step 结束时不重复打印。
                if (advanced)
                    print_battle_stats(
                        s.stats, *s.game, game_stats_label(ctx), s.state.turns);
                return CliResult<void>::Ok();
            }

            CliResult<void> cmd_save(
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

            CliResult<void> cmd_load(
                const Options &opt, const std::filesystem::path &file, Session &s,
                bool ai_explicit, bool hero_explicit)
            {
                if (hero_explicit)
                    return CliFailure{CliError(
                        "load 不支持 --hero（武将随存档恢复）；如需选将请用 "
                        "new --hero <座位>=<武将>")};
                auto text = tkw::io::read_text(file);
                if (text.is_err())
                    return CliFailure{
                        CliError(render_read_error_zh(file, text.unwrap_err()))};
                // 占位建局固定 Brawl：模式与角色由存档恢复，避免以 identity 占位
                // 时因 --players 与存档不符误报 IdentityPlayerCount，或占位洗牌
                // 消耗随机流（随后被 reader 覆盖）。
                auto built = tkw::game::GameFactory::build(
                    build_options_from(opt, tkw::game::GameMode::Brawl));
                if (built.is_err())
                    return CliFailure{CliError(format_build_error(built.unwrap_err()))};
                auto game = std::move(built).unwrap();
                tkw::game::GameSession state;
                tkw::save::SessionMeta meta;
                auto r = tkw::save::read(text.unwrap(), *game, state, &meta);
                if (r.is_err())
                    return CliFailure{CliError(render_save_error_zh(r.unwrap_err()))};
                warn_unsupported_cards(game->catalog);
                warn_unsupported_hero_skills(*game);
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
                s.verbose = session_verbose(opt);
                s.active = true;
                s.deck = opt.deck;
                std::cout << "已加载: " << file.string() << "\n";
                print_status(s);
                return CliResult<void>::Ok();
            }

            CliResult<void> run_game(const Options &opt)
            {
                auto options = build_options_with_heroes(opt);
                if (options.is_err())
                    return CliFailure{CliError(options.unwrap_err())};
                auto built = tkw::game::GameFactory::build(options.unwrap());
                if (built.is_err())
                    return CliFailure{CliError(format_build_error(built.unwrap_err()))};
                auto game = std::move(built).unwrap();
                warn_unsupported_cards(game->catalog);
                warn_unsupported_hero_skills(*game);

                const std::string verr = validate_humans(*game, opt.humans);
                if (!verr.empty())
                    return CliFailure{CliError(verr)};

                // 一次性跑局与建局同口径：真人座位存在且未显式选 verbose 时开日志与回合头。
                const bool verbose = session_verbose(opt);

                auto ctx = game->context();
                auto log = subscribe_event_log(*game, verbose, opt.humans);
                BattleStats stats;
                auto stats_handles = subscribe_stats(*game, stats);

                auto ai = make_decision_source(opt.humans, opt.ai);
                tkw::game::GameSession session;
                if (tkw::game::GameSetup(ctx)
                        .start_session(session, "P0", opt.hand)
                        .is_err())
                    return CliFailure{CliError("开局失败")};
                auto rr = run_to_completion(ctx, *ai, session, nullptr, verbose);
                if (rr.is_err())
                    return CliFailure{CliError(format_loop_error(rr.unwrap_err()))};
                if (rr.unwrap() == RunOutcome::MaxRounds)
                {
                    print_max_rounds_draw(stats, *game, session.turns);
                    return CliResult<void>::Ok();
                }
                std::cout << "胜者: " << game_end_label(ctx)
                          << "，回合数: " << session.turns << "\n";
                print_battle_stats(stats, *game, game_stats_label(ctx), session.turns);
                return CliResult<void>::Ok();
            }

            CliResult<void> audit_deck(const Options &opt)
            {
                const std::string herr = reject_humans(opt.humans, "audit");
                if (!herr.empty())
                    return CliFailure{CliError(herr)};

                auto lines = audit_lines(opt);
                if (lines.is_err())
                    return CliFailure{CliError(lines.unwrap_err())};
                for (const auto &line : lines.unwrap())
                    std::cout << line << "\n";
                return CliResult<void>::Ok();
            }

            CliResult<void> cards_list(const Options &opt, bool show_text)
            {
                const std::string herr = reject_humans(opt.humans, "cards");
                if (!herr.empty())
                    return CliFailure{CliError(herr)};

                auto lines = cards_lines(opt, show_text);
                if (lines.is_err())
                    return CliFailure{CliError(lines.unwrap_err())};
                for (const auto &line : lines.unwrap())
                    std::cout << line << "\n";
                return CliResult<void>::Ok();
            }

            CliResult<void> decks_list(const Options &opt)
            {
                const std::string herr = reject_humans(opt.humans, "decks");
                if (!herr.empty())
                    return CliFailure{CliError(herr)};

                auto lines = decks_lines(opt.deck);
                if (lines.is_err())
                    return CliFailure{CliError(lines.unwrap_err())};
                for (const auto &line : lines.unwrap())
                    std::cout << line << "\n";
                return CliResult<void>::Ok();
            }

            CliResult<void> heroes_list(const Options &opt)
            {
                const std::string herr = reject_humans(opt.humans, "heroes");
                if (!herr.empty())
                    return CliFailure{CliError(herr)};

                auto lines = heroes_lines(opt.deck);
                if (lines.is_err())
                    return CliFailure{CliError(lines.unwrap_err())};
                for (const auto &line : lines.unwrap())
                    std::cout << line << "\n";
                return CliResult<void>::Ok();
            }

            CliResult<void> rules_lookup(
                const Options &opt, const std::string &keyword)
            {
                const std::string herr = reject_humans(opt.humans, "rules");
                if (!herr.empty())
                    return CliFailure{CliError(herr)};

                auto lines = rules_lines(opt, keyword);
                if (lines.is_err())
                    return CliFailure{CliError(lines.unwrap_err())};
                for (const auto &line : lines.unwrap())
                    std::cout << line << "\n";
                return CliResult<void>::Ok();
            }

            SimulateLines simulate_lines(
                const Options &opt, int n,
                std::vector<std::string> *warnings,
                const std::function<bool()> &cancelled)
            {
                // 预载牌表：未实现卡警告由调用方一次性输出，不逐局重复。
                tkw::config::ResourceStore store(opt.deck);
                auto catalog = tkw::card::CardDefCatalog::load(store, "deck");
                if (catalog.is_err())
                    return SimulateLines::Err(format_load_error(catalog.unwrap_err()));
                const auto &cat = catalog.unwrap();

                // 武将选择在批量模拟中对每局相同；--hero 解析错误一次性返回
                auto base_options = build_options_with_heroes(opt);
                if (base_options.is_err())
                    return SimulateLines::Err(base_options.unwrap_err());
                auto hero_catalog = tkw::hero::HeroCatalog::load_optional(
                    store, "heroes");
                if (hero_catalog.is_err())
                    return SimulateLines::Err(
                        format_load_error(hero_catalog.unwrap_err()));
                const auto &hero_cat = hero_catalog.unwrap();
                const tkw::game::BuildOptions base_bo =
                    std::move(base_options).unwrap();

                if (warnings)
                {
                    *warnings = unsupported_cards_warning_lines(cat);
                    const auto hero_warnings =
                        unsupported_hero_skills_warning_lines(hero_cat,
                                                              base_bo.heroes);
                    warnings->insert(warnings->end(), hero_warnings.begin(),
                                     hero_warnings.end());
                }

                SimAggregate agg;
                int completed = 0;
                for (int i = 0; i < n; ++i)
                {
                    if (cancelled && cancelled())
                        break;

                    // 每局独立随机源：种子 = 基种子 + 局序号；武将选择逐局沿用。
                    tkw::game::BuildOptions per = base_bo;
                    per.seed = opt.seed + static_cast<std::uint32_t>(i);
                    auto built = tkw::game::GameFactory::build(per);
                    if (built.is_err())
                        return SimulateLines::Err(
                            format_build_error(built.unwrap_err()));
                    auto game = std::move(built).unwrap();

                    // 全 AI 局：无真人座位，决策源按难度档取单档。
                    auto ctx = game->context();
                    auto ai = make_decision_source({}, opt.ai);
                    tkw::game::GameSession session;
                    if (tkw::game::GameSetup(ctx)
                            .start_session(session, "P0", opt.hand)
                            .is_err())
                        return SimulateLines::Err("开局失败");
                    auto rr = run_to_completion(ctx, *ai, session);
                    if (rr.is_err())
                        return SimulateLines::Err(
                            format_loop_error(rr.unwrap_err()));

                    // 胜者空串 = 平局（达回合上限或同归于尽）；身份局按阵营聚合，
                    // 传空代表 id 得通用阵营标签，避免「内奸胜（P2）」拆成多键。
                    const std::string winner =
                        tkw::game::SessionQuery::session_winner(ctx);
                    if (winner.empty())
                        ++agg.draws;
                    else if (tkw::game::mode_of(ctx) ==
                             tkw::game::GameMode::Identity)
                        ++agg.wins[identity_result_label(
                            tkw::game::SessionQuery::session_camp(ctx), {})];
                    else
                        ++agg.wins[winner];
                    agg.turns_sum += session.turns;
                    ++completed;
                }

                // 一局未完成：仅取消可在首局前到达，给出头行与取消提示。
                if (completed == 0)
                    return SimulateLines::Ok(
                        std::vector<std::string>{
                            "牌表: " + opt.deck.string(),
                            "模拟已取消（完成 0/" + std::to_string(n) + " 局）"});

                // 汇总：头行（牌表来源 + 局数/人数/种子区间/AI 档）+ 逐座位胜场或
                // 身份局阵营胜场与平局 + 平均回合。
                std::vector<std::string> lines;
                lines.push_back("牌表: " + opt.deck.string());
                std::string header =
                    "模拟 " + std::to_string(completed) + " 局（" +
                    std::to_string(opt.players) + " 人，种子 " +
                    std::to_string(opt.seed) + ".." +
                    std::to_string(opt.seed + completed - 1) + "，ai=" +
                    ai_level_name(opt.ai);
                if (opt.mode == tkw::game::GameMode::Identity)
                    header += "，mode=identity";
                header += "）:";
                lines.push_back(std::move(header));

                std::string line;
                if (opt.mode == tkw::game::GameMode::Identity)
                {
                    // 固定阵营序输出，避免依赖中文字符串字典序
                    for (tkw::game::WinCamp c : {tkw::game::WinCamp::LordCamp,
                                                 tkw::game::WinCamp::RebelCamp,
                                                 tkw::game::WinCamp::TraitorCamp})
                    {
                        const std::string label = identity_result_label(c, {});
                        const auto it = agg.wins.find(label);
                        line += (line.empty() ? "" : "，") + label + " " +
                                std::to_string(it == agg.wins.end() ? 0
                                                                    : it->second);
                    }
                }
                else
                {
                    for (int seat = 0; seat < opt.players; ++seat)
                    {
                        const std::string id = "P" + std::to_string(seat);
                        const auto it = agg.wins.find(id);
                        const int w = it == agg.wins.end() ? 0 : it->second;
                        line += (seat == 0 ? "" : "，") + id + " 胜 " +
                                std::to_string(w);
                    }
                }
                line += "，平局 " + std::to_string(agg.draws);
                lines.push_back("  " + line);
                lines.push_back("  平均回合 " +
                                std::to_string(agg.turns_sum / completed));
                return SimulateLines::Ok(std::move(lines));
            }

            CliResult<void> simulate_games(const Options &opt, int n)
            {
                const std::string herr = reject_humans(opt.humans, "simulate");
                if (!herr.empty())
                    return CliFailure{CliError(herr)};
                std::vector<std::string> warnings;
                auto lines = simulate_lines(opt, n, &warnings);
                for (const auto &w : warnings)
                    std::cerr << w << "\n";
                if (lines.is_err())
                    return CliFailure{CliError(lines.unwrap_err())};
                for (const auto &line : lines.unwrap())
                    std::cout << line << "\n";
                return CliResult<void>::Ok();
            }

        }  // namespace detail
    }  // namespace cli
}  // namespace tkw

namespace tkw
{
    namespace cli
    {
        const char *ai_level_name(AiLevel ai)
        {
            for (const auto &entry : kAiLevelTexts)
            {
                if (entry.level == ai)
                    return entry.name;
            }
            return kAiLevelTexts[0].name;
        }

        tkw::Option<AiLevel> ai_level_from(std::string_view name)
        {
            for (const auto &entry : kAiLevelTexts)
            {
                if (name == entry.name)
                    return tkw::Option<AiLevel>::Some(entry.level);
            }
            return tkw::Option<AiLevel>::None();
        }

        void build_app(pjh::cli::App &app, Session &session)
        {
            app.set_help_formatter(
                [](const pjh::cli::BaseCommand &cmd) { return render_help_zh(cmd); });

            const tkw::game::RulesConfig rules{};

            // 根命令选项（无子命令时直接跑一局）；各 leaf 各自声明一份标量选项，
            // 使帮助/用法面与本命令选项段一致，且子命令名之后的选项也可解析；
            // --human 是 repeatable，仅根声明以避免父/叶混写时值分落两处。
            detail::declare_common_options(app, rules);
            detail::declare_human_option(app);
            detail::declare_hero_option(app);

            // 根命令：无子命令时直接跑一局 AI 对局，先打印一行引导。
            app.action(
                [](ParseContext &ctx) -> CliResult<void>
                {
                    std::cout
                        << "（无子命令：跑一局 AI 对局；--help 查看命令，repl 进入交互，"
                           "--human P0 真人参与；想自己玩：tkw --human P0 repl 后先 new 开局）\n";
                    return detail::run_game(detail::options_from(ctx));
                });

            // audit：审计牌堆
            auto &audit = app.add_leaf("audit", "审计牌堆，列出引擎未实现的卡");
            detail::declare_common_options(audit, rules);
            audit.action(
                [&session](ParseContext &ctx) -> CliResult<void>
                {
                    return detail::audit_deck(
                        detail::options_from_for_query(ctx, session));
                });

            // cards：列出牌表（只读牌堆查询，仅 --deck 生效；公共选项与 audit 同款）
            auto &cards =
                app.add_leaf("cards", "列出牌表（牌堆种类与张数；--text 附效果文案）");
            detail::declare_common_options(cards, rules);
            cards.option<fixed_string("text")>(
                     "--text", "在每张卡后附效果说明文案")
                .boolean();
            cards.action(
                [&session](ParseContext &ctx) -> CliResult<void>
                {
                    return detail::cards_list(
                        detail::options_from_for_query(ctx, session),
                        ctx.get_or<bool, fixed_string("text")>(false));
                });

            // decks：列出可用牌表（扫描根目录自身与直接子目录，只读文件系统）
            auto &decks =
                app.add_leaf("decks", "列出可用牌表（预设一览；用 --deck 选择）");
            detail::declare_common_options(decks, rules);
            decks.arg<std::string, 0>(
                "目录", "扫描根目录（缺省取 --deck/会话，否则 resources）");
            decks.action(
                [&session](ParseContext &ctx) -> CliResult<void>
                {
                    Options opt = detail::options_from_for_query(ctx, session);
                    const std::string dir = ctx.get_or<std::string, 0>("");
                    if (!dir.empty())
                        opt.deck = dir;
                    return detail::decks_list(opt);
                });

            // heroes：列出可用武将（读取当前牌表目录的 heroes.json，只读查询）
            auto &heroes =
                app.add_leaf("heroes", "列出可用武将（预设一览；随 --deck 选择）");
            detail::declare_common_options(heroes, rules);
            heroes.arg<std::string, 0>(
                "目录", "武将数据根目录（缺省取 --deck/会话，否则 resources）");
            heroes.action(
                [&session](ParseContext &ctx) -> CliResult<void>
                {
                    Options opt = detail::options_from_for_query(ctx, session);
                    const std::string dir = ctx.get_or<std::string, 0>("");
                    if (!dir.empty())
                        opt.deck = dir;
                    return detail::heroes_list(opt);
                });

            // rules：卡牌效果说明查询（只读牌堆查询，数据源 CardDef.text）
            auto &rules_cmd =
                app.add_leaf("rules", "查询卡牌效果说明：rules [关键词]");
            detail::declare_common_options(rules_cmd, rules);
            rules_cmd.arg<std::string, 0>(
                "关键词", "按卡名/id/效果文案过滤；省略则列出全部");
            rules_cmd.action(
                [&session](ParseContext &ctx) -> CliResult<void>
                {
                    return detail::rules_lookup(
                        detail::options_from_for_query(ctx, session),
                        ctx.get_or<std::string, 0>(""));
                });

            // deal：位置参数跑局（REPL/批量通用）
            auto &deal = app.add_leaf("deal", "跑一局：deal <玩家数> <种子>");
            detail::declare_common_options(deal, rules);
            deal.arg<int, 0>("players", "玩家数").required();
            deal.arg<int, 1>("seed", "随机种子").required();
            deal.action(
                [rules, &session](ParseContext &ctx) -> CliResult<void>
                {
                    Options opt = detail::options_from_for_query(ctx, session);
                    opt.players = ctx.get<int, 0>();
                    opt.seed = static_cast<std::uint32_t>(ctx.get<int, 1>());
                    if (opt.players < rules.min_players ||
                        opt.players > rules.max_players)
                        return CliFailure{
                            CliError(detail::player_range_error(opt.players, rules))};
                    return detail::run_game(opt);
                });

            // simulate：批量模拟（全 AI 跨局聚合；玩家数缺省取启动 --players，否则 4）
            auto &sim = app.add_leaf(
                "simulate", "批量模拟：simulate <局数> [玩家数]");
            detail::declare_common_options(sim, rules);
            sim.arg<int, 0>("n", "局数（≥1）").required();
            sim.arg<int, 1>(
                "players",
                "玩家数（可选；缺省取启动 --players，否则 4）");
            sim.action(
                [rules, &session](ParseContext &ctx) -> CliResult<void>
                {
                    const int n = ctx.get<int, 0>();
                    if (n < 1)
                        return CliFailure{CliError("局数须为正整数")};
                    Options opt = detail::options_from_for_query(ctx, session);
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
                        session, detail::resolve_verbose(ctx, session));
                });

            // run：跑到对局结束（别名 r，REPL 会话流高频命令）
            auto &run_cmd = app.add_leaf("run", "跑到当前会话结束");
            run_cmd.alias("r");
            detail::declare_common_options(run_cmd, rules);
            run_cmd.action(
                [&session](ParseContext &ctx) -> CliResult<void>
                {
                    return detail::cmd_run(
                        session, detail::resolve_verbose(ctx, session));
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
                        ctx.has<fixed_string("ai")>(),
                        ctx.has<fixed_string("hero")>());
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
                    // 进入 REPL 前打印引导：命令列表、退出方式与第一局起手在提示符处不可见。
                    std::cout << "输入 ? 查看命令，help <命令> 看用法，quit 退出\n"
                              << "第一局：new --players 2 --seed 1 开局，再 step；轮到你按 "
                                 "play/pass/discard 提示操作（--verbose 可看每步事件）\n";
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
                        meta.ai = ai_level_name(session.ai);
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
