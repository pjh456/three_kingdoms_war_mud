/**
 * @file   commands.hpp
 * @brief  CLI 命令树与命令执行体。
 * @details 声明公共选项、注册各命令、共享 `Session`；与 `main.cpp` 分离，使命令
 *          树可由测试直接构建并驱动 REPL。命令的 action 写标准输出（用户可见），
 *          框架侧输出（`?`/`help`）走 `InteractiveConsole` 注入的流。
 * @ingroup tkw_cli
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
             * @param[in] ctx  本行命令的解析上下文。
             * @param[in] base 未显式提供时的回落基准。
             * @return 合并后的对局选项。
             * @note REPL 每行命令独立解析，根选项不会自动继承启动命令行的取值，
             *       故 REPL 内建局以启动选项为 base 合并，行内显式选项优先；真人
             *       座位是唯一可清空的累积项，--no-human 显式清空并优先于 --human。
             */
            Options options_from(ParseContext &ctx, const Options &base);

            /**
             * @brief 从解析上下文读参数（无 base：一次性命令使用选项默认值）。
             * @param[in] ctx 本行命令的解析上下文。
             * @return 合并后的对局选项。
             */
            Options options_from(ParseContext &ctx);

            /**
             * @brief 查询/批量命令的选项合并：活动会话牌表优先，无会话回落启动选项；
             *        不继承真人座位。
             * @param[in] ctx     本行命令的解析上下文。
             * @param[in] session 当前会话；base 为 REPL 启动选项，deck 为活动会话牌表。
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
            Options options_from_for_query(
                ParseContext &ctx, const Session &session);

            /**
             * @brief 解析本次命令是否打印事件日志。
             * @param[in] ctx     本次解析上下文。
             * @param[in] session 当前会话；携带建局命令确定的日志开关。
             * @return 本行显式提供 --verbose/--no-verbose 时以显式值为准，否则取
             *         会话值。
             * @note REPL 每行独立解析，启动选项不进本行上下文，故未显式提供时
             *       回落会话默认；显式值只影响本次命令，不改写会话默认，使
             *       --no-verbose 能临时关闭启动带入的日志。
             */
            bool resolve_verbose(ParseContext &ctx, const Session &session);

            /**
             * @brief 建局/一次性跑局时的事件日志默认：真人座位存在且未显式选择过 verbose 时开启。
             * @param[in] opt 合并后的选项；humans/verbose/verbose_explicit 均已就绪。
             * @return 显式提供过（含启动 --no-verbose）→ 取显式值；否则真人局为真、
             *         全 AI 局为假。
             * @note 全 AI 路径保持默认关闭以维持批量/回放输出不变；行内 --no-verbose
             *       只经 resolve_verbose 影响单次命令，不改写本默认。
             */
            bool session_verbose(const Options &opt);

            /**
             * @brief 按选项构造 REPL 历史后端：空路径交回框架默认（内存）。
             * @param[in]     path 历史文件路径；空 = 不持久化。
             * @param[in,out] err  告警输出流；父目录缺失时写一行中文告警。
             * @return FileHistory（父目录存在）或 nullptr（交 InteractiveConsole
             *         回落 InMemoryHistory）。
             * @note FileHistory 路径逐字使用、不建父目录，父目录缺失时写入会
             *       静默失败，故此处预检并显式告警，避免用户误以为已持久化。
             */
            std::unique_ptr<pjh::cli::IHistory> make_repl_history(
                const std::filesystem::path &path, std::ostream &err);

            /**
             * @brief 在命令上声明标量公共选项（牌堆/人数/手牌/种子/日志/存档/历史/AI 难度/对局模式）。
             * @param[in] cmd   目标命令：根命令或会读取这些选项的 leaf。
             * @param[in] rules 玩家数上下限来源。
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
            void declare_common_options(
                pjh::cli::BaseCommand &cmd, const tkw::game::RulesConfig &rules);

            /**
             * @brief 在根命令上声明可重复的真人座位选项与清空开关。
             * @param[in] cmd 目标命令；只应传根命令，使父/叶混写累积进同一上下文。
             * @note repeatable 选项的值按「最近声明」写入单一上下文，若根与 leaf 各
             *       声明一份，`--human P0 new --human P1` 会分落两处，而读取只取最近
             *       节点，导致 P0 静默丢弃；故仅根声明。leaf 处仍可解析（祖先链查找）。
             *       值补全候选取自规则允许的座位上限，防止越界座位号到运行期才报错；
             *       --no-human 是单独的布尔开关（非 --human 的取反），使 REPL 内能
             *       清空由启动选项带入的座位。
             */
            void declare_human_option(pjh::cli::BaseCommand &cmd);

            /**
             * @brief 在根命令上声明可重复的武将选择选项。
             * @param[in] cmd 目标命令；只应传根命令，理由同 declare_human_option。
             * @note 取值形如 `P0=zhangfei`，可重复；不是 negatable，也不提供
             *       --no-hero（武将选择以最近一次显式提供为准）。值补全只列座位
             *       前缀，武将 id 需运行时命中目录，故不做候选静态枚举。仅建局类
             *       命令生效；`load` 行内显式给出会被拒绝（武将随存档恢复）。
             */
            void declare_hero_option(pjh::cli::BaseCommand &cmd);

            /**
             * @brief 建局入参：只取装配所需字段（hand/AI/verbose 属会话参数）。
             * @param[in] opt  命令行选项。
             * @param[in] mode 参与装配的对局模式；load 占位建局固定 Brawl（模式与角色
             *             由存档恢复），其余入口传 opt.mode。
             * @return 建局入参。
             */
            tkw::game::BuildOptions build_options_from(
                const Options &opt, tkw::game::GameMode mode);

            /**
             * @brief 建局入参：对局模式取 opt.mode。
             * @param[in] opt 命令行选项。
             * @return 建局入参。
             */
            tkw::game::BuildOptions build_options_from(const Options &opt);

            /**
             * @brief `--hero` 解析结果：归一化座位 id → 武将 id。
             * @note std::map 在 MSVC 调试构建下的移动不满足 nothrow move，包裹后
             *       显式声明 noexcept 移动，使结果可放入 Result；拷贝保持可用。
             */
            struct HeroAssignments
            {
                std::map<std::string, std::string> by_seat; /**< 座位 id → 武将 id */

                HeroAssignments() = default;
                HeroAssignments(const HeroAssignments &) = default;
                HeroAssignments &operator=(const HeroAssignments &) = default;
                HeroAssignments(HeroAssignments &&other) noexcept
                    : by_seat(std::move(other.by_seat))
                {
                }
                HeroAssignments &operator=(HeroAssignments &&other) noexcept
                {
                    by_seat = std::move(other.by_seat);
                    return *this;
                }
            };

            /**
             * @brief 解析可重复 `--hero` 原文为「座位 id → 武将 id」映射。
             * @param[in] raw     --hero 原始值列表（形如 "P0=zhangfei"）。
             * @param[in] players 本局玩家数，用于座位下标越界校验与可用座位列表。
             * @return Ok 为座位到武将的映射；Err 为中文提示（格式/座位/重复）。
             * @note 座位格式固定 `P<非负十进制>`，解析后归一为规范键 `P<下标>`
             *       （`P00` 与 `P0` 等价），使所有等价写法都能被建局按规范座位
             *       命中，不因键写法差异静默丢弃用户选择；重复经归一化键判定，
             *       报错而非后写覆盖。武将 id 是否存在于目录由 build_game
             *       按当前牌表目录校验（解析期不读文件系统）。
             */
            tkw::Result<HeroAssignments, std::string>
            parse_hero_assignments(
                const std::vector<std::string> &raw, int players);

            /**
             * @brief 建局入参（含武将）：解析 --hero 后并入 BuildOptions。
             * @param[in] opt  命令行选项；heroes 原文与 players 参与解析。
             * @param[in] mode 参与装配的对局模式。
             * @return Ok 为建局入参；Err 为 --hero 中文解析错误（由命令层渲染）。
             */
            tkw::Result<tkw::game::BuildOptions, std::string>
            build_options_with_heroes(
                const Options &opt, tkw::game::GameMode mode);

            /**
             * @brief 建局入参（含武将）：对局模式取 opt.mode。
             * @param[in] opt 命令行选项。
             * @return `Ok` 为建局入参；`Err` 为 --hero 中文解析错误（由命令层渲染）。
             */
            tkw::Result<tkw::game::BuildOptions, std::string>
            build_options_with_heroes(const Options &opt);

            /**
             * @brief 未实现卡警告文本（不含换行）：清单为空时返回空串。
             * @param[in] catalog     已严格加载的牌表目录，用于展示名回落。
             * @param[in] unsupported 引擎未实现的卡 id 列表（game::unsupported_cards 结果）。
             * @return 「警告: 牌堆含 N 张引擎未实现的卡:」+ 每卡 ` <name>(<id>)`；
             *         unsupported 为空时返回空串。
             * @note 纯文本单点：CLI 警告与 TUI 日志共用，避免两处口径漂移。
             */
            std::string unsupported_cards_warning_text(
                const tkw::card::CardDefCatalog &catalog,
                const std::vector<std::string> &unsupported);

            /**
             * @brief 未实现卡警告纯行（0 或 1 行，不含换行）。
             * @param[in] catalog 已严格加载的牌表目录；判定经 game::unsupported_cards。
             * @return 目录全部可结算时为空向量，否则单元素向量。
             * @note 纯函数无输出副作用，供 CLI 逐行打印与 TUI 逐行写日志共用。
             */
            std::vector<std::string> unsupported_cards_warning_lines(
                const tkw::card::CardDefCatalog &catalog);

            /**
             * @brief 打印牌堆中引擎未实现的卡警告（建局与批量入口共用同一口径）。
             * @param[in]     catalog 已严格加载的牌表目录。
             * @param[in,out] err     告警输出流。
             * @note 只覆盖「枚举已存在但结算未实现」；未知机制名在严格加载期即失败，
             *       到不了这里（未知机制的容错审计见 audit）。目录全部可结算时
             *       不输出。建局入口与一次性命令都调用本函数，避免口径漂移。
             */
            void warn_unsupported_cards(
                const tkw::card::CardDefCatalog &catalog,
                std::ostream &err = std::cerr);

            /**
             * @brief 单武将的未实现技能警告文本（不含换行）。
             * @param[in] def 武将定义。
             * @return 「警告: 武将 <名> 含引擎未实现的技能: <技能名、…>」；
             *         全部已实现时为空串。
             */
            std::string unsupported_hero_skill_warning_text(
                const tkw::hero::HeroDef &def);

            /**
             * @brief 已选武将中含引擎未实现技能的警告纯行（0 到多行，不含换行）。
             * @param[in] game 已建好的对局；按实体所绑武将查目录技能实现状态。
             * @return 每名含未实现技能的武将为一行；全部已实现或无武将时为空。
             * @note 只对实际选中的武将告警：未选中的武将数据（含未实现技能）
             *       不产生默认输出，保证无 --hero 的建局输出逐字节不变。
             */
            std::vector<std::string> unsupported_hero_skills_warning_lines(
                const tkw::game::Game &game);

            /**
             * @brief 指定武将 id 集合的未实现技能警告纯行（批量入口用）。
             * @param[in] catalog 武将目录。
             * @param[in] heroes  座位 id → 武将 id 映射；按 id 去重后逐名判定。
             * @return 每名含未实现技能的武将为一行；目录未命中时跳过。
             */
            std::vector<std::string> unsupported_hero_skills_warning_lines(
                const tkw::hero::HeroCatalog &catalog,
                const std::map<std::string, std::string> &heroes);

            /**
             * @brief  打印已选武将中未实现技能的警告。
             * @param[in]     game 已建好的对局；按实体所绑武将查目录技能实现状态。
             * @param[in,out] err  告警输出流。
             * @note   建局入口共用同一口径。
             */
            void warn_unsupported_hero_skills(
                const tkw::game::Game &game, std::ostream &err = std::cerr);

            /**
             * @brief 玩家数越界 → 用户可见文案。
             * @param[in] value 实际传入的玩家数。
             * @param[in] rules 玩家数上下限来源。
             * @return 「玩家数 N 超出范围 [min, max]」，与选项 --players 的解析期
             *         越界文案同用「超出范围 [min, max]」措辞。
             */
            std::string player_range_error(
                int value, const tkw::game::RulesConfig &rules);

            /**
             * @brief 无进行中会话 → 用户可见错误文案（step/run/save 共用单点）。
             * @return 「没有进行中的对局（先运行 new 开局，或进入 tkw repl）」。
             * @note new/load 会建立会话，故引导指向新开局与 repl 两条入口；
             *       退出码由调用方维持 1。
             */
            const char *no_active_game_error();

            /**
             * @brief 校验真人座位：必须是对局中存在的实体且互不重复；空串表示通过。
             * @param[in] game   本局运行时。
             * @param[in] humans 待校验的真人座位集合。
             * @return 查无此 id 时返回「真人座位不存在: <id>（可用座位: ...）」；
             *         重复时返回「真人座位重复: <id>（每个座位只能指定一次）」；
             *         全部通过返回空串。
             * @note 可用座位取当前存活实体的按座位序 id 列表。读档后阵亡者不在
             *       实体集合中，故列表如实反映此刻可选座位，而非 P0..P{n-1} 范围。
             */
            std::string validate_humans(
                tkw::game::Game &game, const std::vector<std::string> &humans);

            /**
             * @brief 不支持真人的命令统一拒绝非空 humans；返回空串表示通过。
             * @param[in] humans 解析/继承得到的真人座位集合。
             * @param[in] cmd    命令名，用于给出可复制的替代出口。
             * @return 空串表示通过；否则为带「直接运行 tkw <cmd>」下一步的中文错误。
             */
            std::string reject_humans(
                const std::vector<std::string> &humans, const std::string &cmd);

            /**
             * @brief 构造决策源：无真人按难度档取 AI，否则按 actor 路由到交互输入
             *        （真人座位外的回落与全 AI 局同一难度档）。
             * @param[in] humans 真人座位 id；空 = 全 AI 对局。
             * @param[in] ai     AI 难度档（决定全 AI 局与真人局回落决策源）。
             * @return 决策源；无真人时为对应难度档 AI，否则为按座位路由的交互输入。
             */
            std::unique_ptr<tkw::game::DecisionSource> make_decision_source(
                const std::vector<std::string> &humans, AiLevel ai);

            /** @brief 跑到底结果：正常结束或达回合上限平局。 */
            enum class RunOutcome : std::uint8_t
            {
                Finished,  /**< 会话结束（乱斗存活 ≤ 1；身份局主公阵亡或敌对尽灭） */
                MaxRounds, /**< 达回合上限且无唯一存活者 */
            };

            /**
             * @brief 回合头文本（不含换行）：回合序号 + 当前玩家。
             * @param[in] session 当前会话进度；turns 为已执行回合数，故本回合 = turns + 1。
             * @return 「—— 回合 N：P ——」。
             * @note 纯文本单点：CLI 打印与 TUI 日志共用，保证两处回合头逐字一致。
             */
            std::string turn_header_text(
                const tkw::game::GameSession &session);

            /**
             * @brief 回合头：打印回合序号与当前玩家，使后续事件可归属。
             * @param[in] session 当前会话进度；turns 为已执行回合数，故下一回合 = turns + 1。
             * @note 仅过程可见（真人默认或 --verbose）时由调用方打印；全 AI 默认
             *       静默路径不得调用，避免污染批量/回放输出。
             */
            void print_turn_header(const tkw::game::GameSession &session);

            /**
             * @brief 重复 step_session 直到会话结束或达回合上限。
             * @param[in,out] ctx     对局运行时；结束判定与逐步推进都作用于其容器。
             * @param[in,out] ai      决策源，由调用方按真人/AI 档构造。
             * @param[in,out] session 会话进度，原地推进。
             * @param[out]    root    非空时透传给 step_session，在回合失败时写回根因。
             * @param[in]     show_turn_headers 为真时每个回合执行前打印回合头（仅过程可见路径）。
             * @param[out]    failed_actor 非空时每轮调用前写入当前角色；回合失败时留下的即
             *        失败角色（失败会推进会话，调用方须在推进前捕获）。
             * @return Ok(Finished) 会话结束；Ok(MaxRounds) 达回合上限平局；
             *         Err 其它 LoopError 原样上抛。
             * @note 只驱动循环，不订阅事件；回合头是唯一可选的打印（默认关闭）。
             */
            tkw::game::LoopResult<RunOutcome> run_to_completion(
                tkw::game::GameContext &ctx, tkw::game::DecisionSource &ai,
                tkw::game::GameSession &session,
                tkw::game::TurnError *root = nullptr,
                bool show_turn_headers = false,
                std::string *failed_actor = nullptr);

            /**
             * @brief 终局「胜者」行标签：乱斗逐字走 winner_label；身份局按阵营。
             * @param[in] ctx 已结束对局的运行时上下文。
             * @return 乱斗=唯一存活者 id（空串回落「平局（同归于尽）」）；
             *         身份局=主公/反贼/内奸阵营标签。
             * @note 与 game_stats_label 拆开：本函数把空胜者渲染为同归于尽平局，
             *       统计块需要保留乱斗原始的「无」口径，二者不可互换。
             */
            std::string game_end_label(const tkw::game::GameContext &ctx);

            /**
             * @brief 统计块「胜者」字段：乱斗保持原始 id（空串=显示「无」）；
             *        身份局用阵营标签。
             * @param[in] ctx 已结束对局的运行时上下文。
             * @return 乱斗=原始胜者 id（空串保持为空）；身份局=阵营标签。
             * @note 乱斗 0 存活时必须回空的原始 id，不能走 game_end_label，否则
             *       统计块会从「胜者: 无」变成「平局（同归于尽）」。
             */
            std::string game_stats_label(const tkw::game::GameContext &ctx);

            /**
             * @brief 把一个牌区渲染为「卡名/卡名」；空区回落「无」。
             * @param[in] ctx  只读上下文，经目录解析展示名（目录可空则回落 def_id）。
             * @param[in] zone 待渲染的牌区副本（手牌/装备区/判定区）。
             * @return 斜杠分隔的中文展示名；zone 为空返回「无」。
             * @note 纯展示，与决策窗口 zone_names 同口径；装备区/判定区为明置信息
             *       可直接传，手牌仅限己方座位传入，不得用于对手手牌。
             */
            std::string zone_names(
                const tkw::game::ReadOnlyContext &ctx,
                const std::vector<tkw::card::Card> &zone);

            /**
             * @brief 打印会话状态：无会话 / 进行中 / 已结束三态。
             * @param[in] s 当前会话；active 为假或 game 为空时只打印「会话: 无」。
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
            void print_status(const Session &s);

            /**
             * @brief 打印回合上限平局行与统计块（循环尾同构两连）。
             * @param[in] stats 本局统计聚合。
             * @param[in] game  本局运行时；统计块读取实体体力。
             * @param[in] turns 已执行回合数。
             * @note 平局无胜者，统计块 winner 传空串（显示「无」）。
             */
            void print_max_rounds_draw(
                const tkw::save::BattleStats &stats, const tkw::game::Game &game,
                int turns);

            CliResult<void> cmd_new(const Options &opt, Session &s);

            CliResult<void> cmd_step(Session &s, bool verbose);

            CliResult<void> cmd_run(Session &s, bool verbose);

            /**
             * @brief 序列化当前会话（含 AI 档与对局统计）并原子写入文件。
             * @param[in]     file 目标存档路径。
             * @param[in,out] s    当前会话。
             * @return Err 无进行中会话 / 写文件失败；成功返回 Ok。
             */
            CliResult<void> cmd_save(
                const std::filesystem::path &file, Session &s);

            /**
             * @brief 读档并落子到会话：恢复存档 AI 档与统计，verbose 不持久化。
             * @param[in]     opt           对局选项；提供占位建局的牌表与玩家数。
             * @param[in]     file          存档路径。
             * @param[in,out] s             目标会话；成功后原地替换。
             * @param[in]     ai_explicit   命令行是否显式给了 --ai；显式值覆盖存档 AI 档。
             * @param[in]     hero_explicit 本行是否显式给了 --hero；显式给出时拒绝（武将
             *                              随存档恢复，无读档换将语义）。
             * @return Err 显式 --hero / 读文件 / 存档解析 / 真人座位校验失败；成功
             *         返回 Ok。
             * @note 旧档无元数据时 AI 档回落命令行取值、统计为空；未知 AI 文本
             *       亦回落命令行，不拒绝存档。模式与角色以存档为准，占位建局固定
             *       Brawl，--mode 在本命令上不生效；武将同样以存档为准，本行
             *       显式 --hero 无换将语义故 fail fast 拒绝（早于读文件，文案给出
             *       替代命令）；REPL 启动 --hero 只是会话默认，不进后续行上下文，
             *       不会触发拒绝。
             */
            CliResult<void> cmd_load(
                const Options &opt, const std::filesystem::path &file, Session &s,
                bool ai_explicit, bool hero_explicit);

            CliResult<void> run_game(const Options &opt);

            /**
             * @brief 审计牌堆：拒绝真人座位后，把 audit_lines 逐行打印到标准输出。
             * @param[in] opt 对局选项；仅 --deck 决定被审计的牌表目录。
             * @return Ok；Err 为牌堆加载失败（kind + detail，与建局错误面一致）。
             * @note 打印包装：human 策略留在本层（--human 拒绝文案与退出行为不变），
             *       行构造与加载复用查询纯函数；行序与换行由本层补齐。
             */
            CliResult<void> audit_deck(const Options &opt);

            /**
             * @brief 列出牌表：拒绝真人座位后，把 cards_lines 逐行打印到标准输出。
             * @param[in] opt       对局选项；仅 --deck 决定被读取的牌表目录。
             * @param[in] show_text 为真时在每卡行末尾附 CardDef.text 效果文案。
             * @return Ok；Err 为牌堆加载失败（kind + detail，与建局错误面一致）。
             * @note 打印包装：human 策略留在本层；只读查询不建局、不消耗随机源，
             *       行构造复用查询纯函数，输出逐字节不变。
             */
            CliResult<void> cards_list(const Options &opt, bool show_text);

            /**
             * @brief 可用牌表一览：拒绝真人座位后，把 decks_lines 逐行打印到标准输出。
             * @param[in] opt 对局选项；仅 deck 作为扫描根目录（位置参数在命令层覆盖）。
             * @return Ok；Err 为扫描根不存在时的中文加载错误。
             * @note 打印包装：human 策略留在本层；只读扫描不建局、不消耗随机源，
             *       行构造复用查询纯函数。
             */
            CliResult<void> decks_list(const Options &opt);

            /**
             * @brief 可用武将一览：拒绝真人座位后，把 heroes_lines 逐行打印到标准输出。
             * @param[in] opt 对局选项；仅 deck 作为武将数据根目录（位置参数在命令层覆盖）。
             * @return Ok；Err 为坏 JSON/未知技能等中文加载错误。
             * @note 打印包装：human 策略留在本层；只读加载不建局、不消耗随机源，
             *       缺 heroes.json 回落空目录而非报错。
             */
            CliResult<void> heroes_list(const Options &opt);

            /**
             * @brief 规则/卡牌说明查询：拒绝真人座位后，把 rules_lines 逐行打印到
             *        标准输出。
             * @param[in] opt     对局选项；仅 --deck 决定被读取的牌表目录。
             * @param[in] keyword 过滤关键词；空串 = 列出全部。
             * @return Ok；Err 为牌堆加载失败（kind + detail，与建局错误面一致）。
             * @note 打印包装：human 策略留在本层；命中谓词与文案回落复用查询纯函数，
             *       只读查询不建局、不消耗随机源。
             */
            CliResult<void> rules_lookup(
                const Options &opt, const std::string &keyword);

            /** @brief 跨局模拟聚合：各座位胜场、平局局数与回合总和（单局展示统计不可跨局累加）。 */
            struct SimAggregate
            {
                std::map<std::string, int> wins; /**< 座位 id → 胜场数 */
                int draws = 0;                  /**< 平局局数（达回合上限或同归于尽） */
                std::int64_t turns_sum = 0;      /**< 全部局回合数总和 */
            };

            /** @brief 批量模拟结果：`Ok` 为汇总行（不含换行），`Err` 为中文错误文案。 */
            using SimulateLines = tkw::Result<std::vector<std::string>, std::string>;

            /**
             * @brief 批量模拟的纯行构造：N 局独立种子全 AI 跑完，返回跨局聚合
             *        摘要行（胜者分布 / 平局 / 平均回合），不打印、不写 stderr。
             * @param[in]  opt       对局选项；seed 为基种子（第 i 局用 seed + i），
             *                       --deck/--players/--hand/--seed/--ai/--hero 生效。
             * @param[in]  n         局数；须 ≥1（由调用方校验），耗时随 n 线性。
             * @param[out] warnings  非空时写入未实现卡与已选武将未实现技能的警告行。
             * @param[in]  cancelled 可选取消谓词；非空且返回 true 时在局边界提前结束。
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
            SimulateLines simulate_lines(
                const Options &opt, int n,
                std::vector<std::string> *warnings = nullptr,
                const std::function<bool()> &cancelled = {});

            /**
             * @brief 批量模拟的 CLI 包装：拒绝真人、逐行打印警告（stderr）与汇总
             *        （stdout），保持 CLI 用户可见输出不变。
             * @param[in] opt 对局选项；seed 为基种子。
             * @param[in] n   局数；须 ≥1（由调用方校验）。
             * @return Ok；Err 为牌堆加载失败 / 开局失败 / 对局失败。
             * @note 先输出警告再输出汇总，与旧「警告在循环前、汇总在后」的合并流
             *       序一致；--human 在全 AI 批量模拟下拒绝。
             */
            CliResult<void> simulate_games(const Options &opt, int n);

        }  // namespace detail

        /**
         * @brief 构建完整命令树并绑定会话。
         * @param[in,out] app     根命令（App）；额外参数策略与中文帮助在此一并设置。
         * @param[in,out] session 跨命令会话，须比 app 的命令存活更久（action 以引用捕获）。
         * @note 命令树与 main() 分离，使测试可构建同一棵树并驱动 InteractiveConsole；
         *       根命令无子命令时跑一局，各 leaf 声明自身可读选项。
         */
        void build_app(pjh::cli::App &app, Session &session);
    }  // namespace cli
}  // namespace tkw

#endif  // INCLUDE_TKW_CLI_COMMANDS_HPP
