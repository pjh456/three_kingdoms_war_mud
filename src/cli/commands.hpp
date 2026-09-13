/**
 * @file commands.hpp
 * @brief CLI 命令树与命令执行体：声明公共选项、注册 12 个命令、共享 Session。
 * @note 与 main.cpp 分离，使命令树可由测试直接构建并驱动 REPL。命令的
 *       action 写标准输出（用户可见），框架侧输出（?/help）走 InteractiveConsole
 *       注入的流。
 */
#ifndef INCLUDE_TKW_CLI_COMMANDS_HPP
#define INCLUDE_TKW_CLI_COMMANDS_HPP

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
#include <pjh_cli/console/file_history.hpp>
#include <pjh_cli/console/help_navigator.hpp>
#include <pjh_cli/console/query_result.hpp>

#include "card/card.hpp"
#include "card/catalog.hpp"
#include "cli/error_zh.hpp"
#include "cli/help_zh.hpp"
#include "cli/query_lines.hpp"
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
#include "game/ai/view.hpp"
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

            /** 从解析上下文读参数（无 base：一次性命令使用选项默认值）。 */
            inline Options options_from(ParseContext &ctx)
            {
                return options_from(ctx, Options{});
            }

            /**
             * @brief 查询/批量命令的选项合并：活动会话牌表优先，无会话回落启动选项；
             *        不继承真人座位。
             * @param ctx     本行命令的解析上下文。
             * @param session 当前会话；base 为 REPL 启动选项，deck 为活动会话牌表。
             * @return 合并后的 Options：行内显式值优先，否则回落会话/启动默认。
             *         默认 deck 在存在活动会话（active 且 game 非空）时取
             *         session.deck，否则取 session.base.deck，两者均由行内 --deck
             *         覆盖；humans 先清空再按行内 --human/--no-human 合并。
             * @note 活动会话优先使查询/批量命令与 status 展示的牌表来源一致；无活动
             *       会话（批量路径或 REPL 未 new/load）时 deck 取 base.deck，与「继承
             *       启动 --deck」逐字等价，不静默回落缺省 resources。只读/批量命令不
             *       运行真人局，若继承启动 --human 会被 reject_humans 误拒，故 humans
             *       只隔离、不清除行内值。
             */
            inline Options options_from_for_query(
                ParseContext &ctx, const Session &session)
            {
                Options seed = session.base;
                // 活动会话存在时以会话牌表为默认上下文，使牌表来源可追溯到用户最近一次建局。
                if (session.active && session.game)
                    seed.deck = session.deck;

                seed.humans.clear();
                return options_from(ctx, seed);
            }

            /**
             * @brief 解析本次命令是否打印事件日志。
             * @param ctx     本次解析上下文。
             * @param session 当前会话；携带建局命令确定的日志开关。
             * @return 本行显式提供 --verbose/--no-verbose 时以显式值为准，否则取
             *         会话值。
             * @note REPL 每行独立解析，启动选项不进本行上下文，故未显式提供时
             *       回落会话默认；显式值只影响本次命令，不改写会话默认，使
             *       --no-verbose 能临时关闭启动带入的日志。
             */
            inline bool resolve_verbose(ParseContext &ctx, const Session &session)
            {
                return ctx.was_provided<fixed_string("verbose")>()
                           ? ctx.get<bool, fixed_string("verbose")>()
                           : session.verbose;
            }

            /**
             * @brief 建局/一次性跑局时的事件日志默认：真人座位存在且未显式选择过 verbose 时开启。
             * @param opt 合并后的选项；humans/verbose/verbose_explicit 均已就绪。
             * @return 显式提供过（含启动 --no-verbose）→ 取显式值；否则真人局为真、
             *         全 AI 局为假。
             * @note 全 AI 路径保持默认关闭以维持批量/回放输出不变；行内 --no-verbose
             *       只经 resolve_verbose 影响单次命令，不改写本默认。
             */
            inline bool session_verbose(const Options &opt)
            {
                if (opt.verbose_explicit)
                    return opt.verbose;
                return opt.verbose || !opt.humans.empty();
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
             * @brief 在命令上声明标量公共选项（牌堆/人数/手牌/种子/日志/存档/历史/AI 难度/对局模式）。
             * @param cmd   目标命令：根命令或会读取这些选项的 leaf。
             * @param rules 玩家数上下限来源。
             * @note pjh_cli 的选项查找沿父链（名与值都取最近声明处），故 leaf 不重
             *       声明也能解析祖先的选项；此处 per-leaf 重声明只为让 leaf 的帮助/
             *       用法行列出这些选项。未显式给的项仍由 options_from 沿父链或会话
             *       启动选项回落；标量「最近声明胜出」即期望语义，故 per-leaf 重声明
             *       无副作用。默认值只以描述文字标注，禁用 .default_value()：它会让
             *       parse_finalizer 每次解析把默认值写进上下文，压过 REPL 启动选项
             *       （session.base），破坏会话继承。--verbose 标 negatable 以支持
             *       --no-verbose 关闭；是否采用显式值由 was_provided 判定，未提供
             *       时回落会话默认。--ai 走 enum 映射，值域外输入在解析期报
             *       enum_value_error（与未知选项同一 rc=2 错误面）。--mode 同
             *       enum 范式，默认 brawl 只写进描述文字；load 的占位建局固定
             *       brawl，模式与角色以存档为准，故 --mode 在 load 上不生效。
             */
            inline void declare_common_options(
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
                    .mapping({{"simple", AiLevel::Simple},
                             {"aggressive", AiLevel::Aggressive}})
                    .completer([] {
                        // 候选值域与上方 enum 映射保持一致
                        return std::vector<std::string>{"simple", "aggressive"};
                    });
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

            /**
             * @brief 在根命令上声明可重复的武将选择选项。
             * @param cmd 目标命令；只应传根命令，理由同 declare_human_option。
             * @note 取值形如 `P0=zhangfei`，可重复；不是 negatable，也不提供
             *       --no-hero（武将选择以最近一次显式提供为准）。值补全只列座位
             *       前缀，武将 id 需运行时命中目录，故不做候选静态枚举。
             */
            inline void declare_hero_option(pjh::cli::BaseCommand &cmd)
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

            /**
             * @brief 建局入参：只取装配所需字段（hand/AI/verbose 属会话参数）。
             * @param opt  命令行选项。
             * @param mode 参与装配的对局模式；load 占位建局固定 Brawl（模式与角色
             *             由存档恢复），其余入口传 opt.mode。
             */
            inline tkw::game::BuildOptions build_options_from(
                const Options &opt, tkw::game::GameMode mode)
            {
                return tkw::game::BuildOptions{opt.deck, opt.players, opt.seed, mode};
            }

            /** 建局入参：对局模式取 opt.mode。 */
            inline tkw::game::BuildOptions build_options_from(const Options &opt)
            {
                return build_options_from(opt, opt.mode);
            }

            /**
             * @brief 解析可重复 `--hero` 原文为「座位 id → 武将 id」映射。
             * @param raw     --hero 原始值列表（形如 "P0=zhangfei"）。
             * @param players 本局玩家数，用于座位下标越界校验与可用座位列表。
             * @return Ok 为座位到武将的映射；Err 为中文提示（格式/座位/重复）。
             * @note 座位格式固定 `P<非负十进制>`，解析后归一为规范键 `P<下标>`
             *       （`P00` 与 `P0` 等价），使所有等价写法都能被建局按规范座位
             *       命中，不因键写法差异静默丢弃用户选择；重复经归一化键判定，
             *       报错而非后写覆盖。武将 id 是否存在于目录由 build_game
             *       按当前牌表目录校验（解析期不读文件系统）。
             */
            inline tkw::Result<std::map<std::string, std::string>, std::string>
            parse_hero_assignments(
                const std::vector<std::string> &raw, int players)
            {
                std::map<std::string, std::string> out;
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
                        return tkw::Result<std::map<std::string, std::string>,
                                           std::string>::Err(
                            "武将选项格式须为 座位=武将（如 P0=zhangfei）: " + item);

                    const std::string seat = item.substr(0, eq);
                    const std::string hero_id = item.substr(eq + 1);
                    if (seat.size() < 2 || seat.front() != 'P')
                        return tkw::Result<std::map<std::string, std::string>,
                                           std::string>::Err(
                            "武将座位须形如 P0: " + item);

                    int index = 0;
                    const char *begin = seat.data() + 1;
                    const char *end = seat.data() + seat.size();
                    const auto r = std::from_chars(begin, end, index);
                    if (r.ec != std::errc{} || r.ptr != end)
                        return tkw::Result<std::map<std::string, std::string>,
                                           std::string>::Err(
                            "武将座位须形如 P0: " + item);
                    if (index < 0 || index >= players)
                        return tkw::Result<std::map<std::string, std::string>,
                                           std::string>::Err(
                            "武将座位超出玩家数: " + seat + "（当前 " +
                            std::to_string(players) + " 人）" + seat_hint);

                    const std::string canonical = "P" + std::to_string(index);
                    if (!out.emplace(canonical, hero_id).second)
                        return tkw::Result<std::map<std::string, std::string>,
                                           std::string>::Err(
                            "武将座位重复: " + canonical +
                            "（每个座位只能指定一次）");
                }
                return tkw::Result<std::map<std::string, std::string>,
                                   std::string>::Ok(std::move(out));
            }

            /**
             * @brief 建局入参（含武将）：解析 --hero 后并入 BuildOptions。
             * @param opt  命令行选项；heroes 原文与 players 参与解析。
             * @param mode 参与装配的对局模式。
             * @return Ok 为建局入参；Err 为 --hero 中文解析错误（由命令层渲染）。
             */
            inline tkw::Result<tkw::game::BuildOptions, std::string>
            build_options_with_heroes(
                const Options &opt, tkw::game::GameMode mode)
            {
                auto parsed = parse_hero_assignments(opt.heroes, opt.players);
                if (parsed.is_err())
                    return tkw::Result<tkw::game::BuildOptions,
                                       std::string>::Err(parsed.unwrap_err());
                auto bo = build_options_from(opt, mode);
                bo.heroes = std::move(parsed).unwrap();
                return tkw::Result<tkw::game::BuildOptions, std::string>::Ok(
                    std::move(bo));
            }

            /** 建局入参（含武将）：对局模式取 opt.mode。 */
            inline tkw::Result<tkw::game::BuildOptions, std::string>
            build_options_with_heroes(const Options &opt)
            {
                return build_options_with_heroes(opt, opt.mode);
            }

            /**
             * @brief 未实现卡警告文本（不含换行）：清单为空时返回空串。
             * @param catalog     已严格加载的牌表目录，用于展示名回落。
             * @param unsupported 引擎未实现的卡 id 列表（game::unsupported_cards 结果）。
             * @return 「警告: 牌堆含 N 张引擎未实现的卡:」+ 每卡 ` <name>(<id>)`；
             *         unsupported 为空时返回空串。
             * @note 纯文本单点：CLI 警告与 TUI 日志共用，避免两处口径漂移。
             */
            inline std::string unsupported_cards_warning_text(
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

            /**
             * @brief 未实现卡警告纯行（0 或 1 行，不含换行）。
             * @param catalog 已严格加载的牌表目录；判定经 game::unsupported_cards。
             * @return 目录全部可结算时为空向量，否则单元素向量。
             * @note 纯函数无输出副作用，供 CLI 逐行打印与 TUI 逐行写日志共用。
             */
            inline std::vector<std::string> unsupported_cards_warning_lines(
                const tkw::card::CardDefCatalog &catalog)
            {
                const std::string text = unsupported_cards_warning_text(
                    catalog, tkw::game::unsupported_cards(catalog));
                return text.empty() ? std::vector<std::string>{}
                                    : std::vector<std::string>{text};
            }

            /**
             * @brief 打印牌堆中引擎未实现的卡警告（建局与批量入口共用同一口径）。
             * @param catalog 已严格加载的牌表目录。
             * @param err     告警输出流。
             * @note 只覆盖「枚举已存在但结算未实现」；未知机制名在严格加载期即失败，
             *       到不了这里（未知机制的容错审计见 audit）。目录全部可结算时
             *       不输出。建局入口与一次性命令都调用本函数，避免口径漂移。
             */
            inline void warn_unsupported_cards(
                const tkw::card::CardDefCatalog &catalog,
                std::ostream &err = std::cerr)
            {
                for (const auto &line : unsupported_cards_warning_lines(catalog))
                    err << line << "\n";
            }

            /**
             * @brief 单武将的未实现技能警告文本（不含换行）。
             * @param def 武将定义。
             * @return 「警告: 武将 <名> 含引擎未实现的技能: <技能名、…>」；
             *         全部已实现时为空串。
             */
            inline std::string unsupported_hero_skill_warning_text(
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

            /**
             * @brief 已选武将中含引擎未实现技能的警告纯行（0 到多行，不含换行）。
             * @param game 已建好的对局；按实体所绑武将查目录技能实现状态。
             * @return 每名含未实现技能的武将为一行；全部已实现或无武将时为空。
             * @note 只对实际选中的武将告警：未选中的武将数据（含未实现技能）
             *       不产生默认输出，保证无 --hero 的建局输出逐字节不变。
             */
            inline std::vector<std::string> unsupported_hero_skills_warning_lines(
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

            /**
             * @brief 指定武将 id 集合的未实现技能警告纯行（批量入口用）。
             * @param catalog 武将目录。
             * @param heroes  座位 id → 武将 id 映射；按 id 去重后逐名判定。
             * @return 每名含未实现技能的武将为一行；目录未命中时跳过。
             */
            inline std::vector<std::string> unsupported_hero_skills_warning_lines(
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

            /** @brief 打印已选武将中未实现技能的警告（建局入口共用同一口径）。 */
            inline void warn_unsupported_hero_skills(
                const tkw::game::Game &game, std::ostream &err = std::cerr)
            {
                for (const auto &line : unsupported_hero_skills_warning_lines(game))
                    err << line << "\n";
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

            /**
             * @brief 不支持真人的命令统一拒绝非空 humans；返回空串表示通过。
             * @param humans 解析/继承得到的真人座位集合。
             * @param cmd    命令名，用于给出可复制的替代出口。
             * @return 空串表示通过；否则为带「直接运行 tkw <cmd>」下一步的中文错误。
             */
            inline std::string reject_humans(
                const std::vector<std::string> &humans, const std::string &cmd)
            {
                if (humans.empty())
                    return {};
                return cmd +
                       " 不支持 --human（该命令不运行真人参与的对局）；请直接运行 tkw " +
                       cmd;
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
                Finished,  /**< 会话结束（乱斗存活 ≤ 1；身份局主公阵亡或敌对尽灭） */
                MaxRounds, /**< 达回合上限且无唯一存活者 */
            };

            /**
             * @brief 回合头文本（不含换行）：回合序号 + 当前玩家。
             * @param session 当前会话进度；turns 为已执行回合数，故本回合 = turns + 1。
             * @return 「—— 回合 N：P ——」。
             * @note 纯文本单点：CLI 打印与 TUI 日志共用，保证两处回合头逐字一致。
             */
            inline std::string turn_header_text(
                const tkw::game::GameSession &session)
            {
                return "—— 回合 " + std::to_string(session.turns + 1) + "：" +
                       session.current + " ——";
            }

            /**
             * @brief 回合头：打印回合序号与当前玩家，使后续事件可归属。
             * @param session 当前会话进度；turns 为已执行回合数，故下一回合 = turns + 1。
             * @note 仅过程可见（真人默认或 --verbose）时由调用方打印；全 AI 默认
             *       静默路径不得调用，避免污染批量/回放输出。
             */
            inline void print_turn_header(const tkw::game::GameSession &session)
            {
                std::cout << turn_header_text(session) << "\n";
            }

            /**
             * @brief 重复 step_session 直到会话结束或达回合上限。
             * @param ctx     对局运行时；结束判定与逐步推进都作用于其容器。
             * @param ai      决策源，由调用方按真人/AI 档构造。
             * @param session 会话进度，原地推进。
             * @param root    非空时透传给 step_session，在回合失败时写回根因。
             * @param show_turn_headers 为真时每个回合执行前打印回合头（仅过程可见路径）。
             * @param failed_actor 非空时每轮调用前写入当前角色；回合失败时留下的即
             *        失败角色（失败会推进会话，调用方须在推进前捕获）。
             * @return Ok(Finished) 会话结束；Ok(MaxRounds) 达回合上限平局；
             *         Err 其它 LoopError 原样上抛。
             * @note 只驱动循环，不订阅事件；回合头是唯一可选的打印（默认关闭）。
             */
            inline tkw::game::LoopResult<RunOutcome> run_to_completion(
                tkw::game::GameContext &ctx, tkw::game::DecisionSource &ai,
                tkw::game::GameSession &session,
                tkw::game::TurnError *root = nullptr,
                bool show_turn_headers = false,
                std::string *failed_actor = nullptr)
            {
                while (!tkw::game::session_over(ctx))
                {
                    if (show_turn_headers)
                        print_turn_header(session);
                    if (failed_actor)
                        *failed_actor = session.current;
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
             * @brief 终局「胜者」行标签：乱斗逐字走 winner_label；身份局按阵营。
             * @param ctx 已结束对局的运行时上下文。
             * @return 乱斗=唯一存活者 id（空串回落「平局（同归于尽）」）；
             *         身份局=主公/反贼/内奸阵营标签。
             * @note 与 game_stats_label 拆开：本函数把空胜者渲染为同归于尽平局，
             *       统计块需要保留乱斗原始的「无」口径，二者不可互换。
             */
            inline std::string game_end_label(const tkw::game::GameContext &ctx)
            {
                if (tkw::game::mode_of(ctx) == tkw::game::GameMode::Brawl)
                    return winner_label(tkw::game::session_winner(ctx));
                return identity_result_label(
                    tkw::game::session_camp(ctx), tkw::game::session_winner(ctx));
            }

            /**
             * @brief 统计块「胜者」字段：乱斗保持原始 id（空串=显示「无」）；
             *        身份局用阵营标签。
             * @param ctx 已结束对局的运行时上下文。
             * @note 乱斗 0 存活时必须回空的原始 id，不能走 game_end_label，否则
             *       统计块会从「胜者: 无」变成「平局（同归于尽）」。
             */
            inline std::string game_stats_label(const tkw::game::GameContext &ctx)
            {
                if (tkw::game::mode_of(ctx) == tkw::game::GameMode::Brawl)
                    return tkw::game::session_winner(ctx);
                return identity_result_label(
                    tkw::game::session_camp(ctx), tkw::game::session_winner(ctx));
            }

            /**
             * @brief 把一个牌区渲染为「卡名/卡名」；空区回落「无」。
             * @param ctx  只读上下文，经目录解析展示名（目录可空则回落 def_id）。
             * @param zone 待渲染的牌区副本（手牌/装备区/判定区）。
             * @return 斜杠分隔的中文展示名；zone 为空返回「无」。
             * @note 纯展示，与决策窗口 zone_names 同口径；装备区/判定区为明置信息
             *       可直接传，手牌仅限己方座位传入，不得用于对手手牌。
             */
            inline std::string zone_names(
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

            /**
             * @brief 打印会话状态：无会话 / 进行中 / 已结束三态。
             * @param s 当前会话；active 为假或 game 为空时只打印「会话: 无」。
             * @note 结束态以引擎 session_over（乱斗存活 ≤ 1；身份局主公阵亡或
             *       敌对尽灭）判定，胜者经 game_end_label（乱斗 winner_label，
             *       身份局阵营标签）；仅进行中打印「下一回合」与牌表来源，已达
             *       回合上限但未终结时追加一行提示。身份局额外打印模式行与逐座
             *       角色，乱斗分支不新增任何行；有真人参与且未终局时角色收敛为
             *       主公与真人座位可见、其余占位「未知」，全 AI 局与终局公开
             *       全部角色。局面段中 `s.humans` 命中的座位手牌字段经与决策窗口
             *       同一边界展开为己方牌名，其余座位仍只给数量；装备区/判定区为
             *       明置信息，任何座位均展开牌名，空区回落「无」。武将身份公开：
             *       仅当任一实体有武将时，逐座在角色后、横置前追加「武将 <名|无>」，
             *       无 --hero 的默认局面段逐字节不变。
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
                auto options = build_options_with_heroes(opt);
                if (options.is_err())
                    return CliFailure{CliError(options.unwrap_err())};
                auto built = tkw::game::build_game(options.unwrap());
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
                if (tkw::game::start_session(ctx, state, "P0", opt.hand).is_err())
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

            inline CliResult<void> cmd_step(Session &s, bool verbose)
            {
                if (!s.active || !s.game)
                    return CliFailure{CliError(no_active_game_error())};
                auto log = subscribe_event_log(*s.game, verbose, s.humans);
                auto stats_handles = subscribe_stats(*s.game, s.stats);
                auto ai = make_decision_source(s.humans, s.ai);
                auto ctx = s.game->context();
                if (tkw::game::session_over(ctx))
                {
                    std::cout << "对局已结束，胜者: " << game_end_label(ctx) << "\n";
                    return CliResult<void>::Ok();
                }
                if (verbose)
                    print_turn_header(s.state);
                tkw::game::TurnError root = tkw::game::TurnError::PlayRejected;
                const std::string actor = s.state.current;  // 失败会推进，须先捕获
                auto r = tkw::game::step_session(ctx, *ai, s.state, &root);
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
                if (tkw::game::session_over(ctx))
                {
                    std::cout << "对局结束，胜者: " << game_end_label(ctx) << "\n";
                    print_battle_stats(
                        s.stats, *s.game, game_stats_label(ctx), s.state.turns);
                }
                else
                    print_status(s);
                return CliResult<void>::Ok();
            }

            inline CliResult<void> cmd_run(Session &s, bool verbose)
            {
                if (!s.active || !s.game)
                    return CliFailure{CliError(no_active_game_error())};
                auto log = subscribe_event_log(*s.game, verbose, s.humans);
                auto stats_handles = subscribe_stats(*s.game, s.stats);
                auto ai = make_decision_source(s.humans, s.ai);
                auto ctx = s.game->context();
                // 进入循环前判定：true 表示本次命令至少会推进（用于末尾统计门控）。
                const bool advanced = !tkw::game::session_over(ctx);
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
             *       亦回落命令行，不拒绝存档。模式与角色以存档为准，占位建局固定
             *       Brawl，--mode 在本命令上不生效。
             */
            inline CliResult<void> cmd_load(
                const Options &opt, const std::filesystem::path &file, Session &s,
                bool ai_explicit)
            {
                auto text = tkw::io::read_text(file);
                if (text.is_err())
                    return CliFailure{
                        CliError(render_read_error_zh(file, text.unwrap_err()))};
                // 占位建局固定 Brawl：模式与角色由存档恢复，避免以 identity 占位
                // 时因 --players 与存档不符误报 IdentityPlayerCount，或占位洗牌
                // 消耗随机流（随后被 reader 覆盖）。
                auto built = tkw::game::build_game(
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

            inline CliResult<void> run_game(const Options &opt)
            {
                auto options = build_options_with_heroes(opt);
                if (options.is_err())
                    return CliFailure{CliError(options.unwrap_err())};
                auto built = tkw::game::build_game(options.unwrap());
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
                if (tkw::game::start_session(ctx, session, "P0", opt.hand).is_err())
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

            /**
             * @brief 审计牌堆：拒绝真人座位后，把 audit_lines 逐行打印到标准输出。
             * @param opt 对局选项；仅 --deck 决定被审计的牌表目录。
             * @return Ok；Err 为牌堆加载失败（kind + detail，与建局错误面一致）。
             * @note 打印包装：human 策略留在本层（--human 拒绝文案与退出行为不变），
             *       行构造与加载复用查询纯函数；行序与换行由本层补齐。
             */
            inline CliResult<void> audit_deck(const Options &opt)
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

            /**
             * @brief 列出牌表：拒绝真人座位后，把 cards_lines 逐行打印到标准输出。
             * @param opt       对局选项；仅 --deck 决定被读取的牌表目录。
             * @param show_text 为真时在每卡行末尾附 CardDef.text 效果文案。
             * @return Ok；Err 为牌堆加载失败（kind + detail，与建局错误面一致）。
             * @note 打印包装：human 策略留在本层；只读查询不建局、不消耗随机源，
             *       行构造复用查询纯函数，输出逐字节不变。
             */
            inline CliResult<void> cards_list(const Options &opt, bool show_text)
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

            /**
             * @brief 可用牌表一览：拒绝真人座位后，把 decks_lines 逐行打印到标准输出。
             * @param opt 对局选项；仅 deck 作为扫描根目录（位置参数在命令层覆盖）。
             * @return Ok；Err 为扫描根不存在时的中文加载错误。
             * @note 打印包装：human 策略留在本层；只读扫描不建局、不消耗随机源，
             *       行构造复用查询纯函数。
             */
            inline CliResult<void> decks_list(const Options &opt)
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

            /**
             * @brief 可用武将一览：拒绝真人座位后，把 heroes_lines 逐行打印到标准输出。
             * @param opt 对局选项；仅 deck 作为武将数据根目录（位置参数在命令层覆盖）。
             * @return Ok；Err 为坏 JSON/未知技能等中文加载错误。
             * @note 打印包装：human 策略留在本层；只读加载不建局、不消耗随机源，
             *       缺 heroes.json 回落空目录而非报错。
             */
            inline CliResult<void> heroes_list(const Options &opt)
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

            /**
             * @brief 规则/卡牌说明查询：拒绝真人座位后，把 rules_lines 逐行打印到
             *        标准输出。
             * @param opt     对局选项；仅 --deck 决定被读取的牌表目录。
             * @param keyword 过滤关键词；空串 = 列出全部。
             * @return Ok；Err 为牌堆加载失败（kind + detail，与建局错误面一致）。
             * @note 打印包装：human 策略留在本层；命中谓词与文案回落复用查询纯函数，
             *       只读查询不建局、不消耗随机源。
             */
            inline CliResult<void> rules_lookup(
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

            /** 跨局模拟聚合：各座位胜场、平局局数与回合总和（单局展示统计不可跨局累加）。 */
            struct SimAggregate
            {
                std::map<std::string, int> wins; /**< 座位 id → 胜场数 */
                int draws = 0;                  /**< 平局局数（达回合上限或同归于尽） */
                std::int64_t turns_sum = 0;      /**< 全部局回合数总和 */
            };

            /** 批量模拟结果：Ok 为汇总行（不含换行），Err 为中文错误文案。 */
            using SimulateLines = tkw::Result<std::vector<std::string>, std::string>;

            /**
             * @brief 批量模拟的纯行构造：N 局独立种子全 AI 跑完，返回跨局聚合
             *        摘要行（胜者分布 / 平局 / 平均回合），不打印、不写 stderr。
             * @param opt       对局选项；seed 为基种子（第 i 局用 seed + i），
             *                  --deck/--players/--hand/--seed/--ai/--hero 生效。
             * @param n         局数；须 ≥1（由调用方校验），耗时随 n 线性。
             * @param warnings  非空时写入未实现卡与已选武将未实现技能的警告行。
             * @param cancelled 可选取消谓词；非空且返回 true 时在局边界提前结束。
             * @return Ok 汇总行序（牌表头 + 模拟头 + 胜场/阵营行 + 平均回合）；
             *         Err 为牌堆加载失败 / 开局失败 / 对局失败（文案与 run_game 一致）。
             * @note 基种子缺省由调用方定（CLI/TUI 均 1，局种子 1..N）；每局经
             *       build_game 自建独立 Game 与 SeededRng，绝不读写调用方
             *       session/Game/rng；不订阅事件、不打回合头与统计块。取消时按已
             *       完成局数输出；一局未完成（仅取消路径可达）返回头行与取消提示。
             *       汇总头行先亮明本批实际使用的牌表目录；胜者空串 = 平局；身份局
             *       聚合键换阵营标签并按主公/反贼/内奸固定序输出，头行追加
             *       mode=identity；平均回合为回合总和除以已完成局数（向下取整）。
             */
            inline SimulateLines simulate_lines(
                const Options &opt, int n,
                std::vector<std::string> *warnings = nullptr,
                const std::function<bool()> &cancelled = {})
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
                    auto built = tkw::game::build_game(per);
                    if (built.is_err())
                        return SimulateLines::Err(
                            format_build_error(built.unwrap_err()));
                    auto game = std::move(built).unwrap();

                    // 全 AI 局：无真人座位，决策源按难度档取单档。
                    auto ctx = game->context();
                    auto ai = make_decision_source({}, opt.ai);
                    tkw::game::GameSession session;
                    if (tkw::game::start_session(ctx, session, "P0", opt.hand)
                            .is_err())
                        return SimulateLines::Err("开局失败");
                    auto rr = run_to_completion(ctx, *ai, session);
                    if (rr.is_err())
                        return SimulateLines::Err(
                            format_loop_error(rr.unwrap_err()));

                    // 胜者空串 = 平局（达回合上限或同归于尽）；身份局按阵营聚合，
                    // 传空代表 id 得通用阵营标签，避免「内奸胜（P2）」拆成多键。
                    const std::string winner = tkw::game::session_winner(ctx);
                    if (winner.empty())
                        ++agg.draws;
                    else if (tkw::game::mode_of(ctx) ==
                             tkw::game::GameMode::Identity)
                        ++agg.wins[identity_result_label(
                            tkw::game::session_camp(ctx), {})];
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

            /**
             * @brief 批量模拟的 CLI 包装：拒绝真人、逐行打印警告（stderr）与汇总
             *        （stdout），保持 CLI 用户可见输出不变。
             * @param opt 对局选项；seed 为基种子。
             * @param n   局数；须 ≥1（由调用方校验）。
             * @return Ok；Err 为牌堆加载失败 / 开局失败 / 对局失败。
             * @note 先输出警告再输出汇总，与旧「警告在循环前、汇总在后」的合并流
             *       序一致；--human 在全 AI 批量模拟下拒绝。
             */
            inline CliResult<void> simulate_games(const Options &opt, int n)
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
