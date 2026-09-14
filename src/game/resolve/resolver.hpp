/**
 * @file resolver.hpp
 * @brief 效果结算器：按 CardEffectKind 分派，把「打出的牌」作用于对局状态。
 * @details 只实现核心结算（杀/闪/桃/无中生有/过拆/顺牵/南蛮/万箭/决斗/桃园/
 *          五谷/借刀）；无懈/延时等返回 UnsupportedKind，作为明确未接缝；
 *          装备走 equip_card，不经本文件效果结算。
 * @note 响应牌消费与效果落子在本模块（单一写者）；只读的目标/距离校验见
 *       validate.hpp，出牌动作枚举见 ai/legal.hpp，玩家主观选择经
 *       DecisionSource 询问。结算校验先于消费：失败不消耗打出的牌。
 * @ingroup tkw_game_resolve
 */

#ifndef INCLUDE_TKW_GAME_RESOLVER_HPP
#define INCLUDE_TKW_GAME_RESOLVER_HPP

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "card/catalog.hpp"
#include "card/def.hpp"
#include "card/manager.hpp"
#include "entity/manager.hpp"
#include "event/event_bus.hpp"
#include "game/core/context.hpp"
#include "game/core/decision.hpp"
#include "game/core/effect.hpp"
#include "game/core/state.hpp"
#include "game/query/distance.hpp"
#include "game/query/equip.hpp"
#include "game/query/hero.hpp"
#include "game/query/judge.hpp"
#include "game/resolve/combat.hpp"
#include "game/resolve/counter.hpp"
#include "game/resolve/response.hpp"
#include "game/resolve/validate.hpp"
#include "game/resolve/weapon.hpp"
#include "util/types.hpp"

namespace tkw
{
    namespace game
    {
        // 效果类别属性（is_settleable_kind / is_unimplemented_active_kind）见 effect.hpp

        /**
         * @brief 开杀响应窗口并消费响应杀。
         * @details 响应者打出一张真杀、将一张转化来源牌当杀打出（武圣红牌 /
         *          龙胆闪），或（装备两张当杀能力时）打出两张手牌当杀。消费在本
         *          函数内完成（弃置+事件）；给出结算目标（借刀的 B）时再按杀对其
         *          结算（真杀带花色、虚拟杀无花色），否则仅消费（决斗/南蛮无结算
         *          目标）。
         * @param[in] ctx    对局上下文。
         * @param[in] ai     决策源；询问是否响应及打出的牌。
         * @param[in] entity 被询问的实体 id。
         * @param[in] victim 结算目标 id；空串 = 仅消费（决斗/南蛮）。
         * @param[in] prompt 响应来源与后果（只读事实，透传决策源）。
         * @return 是否发生了有效杀响应。
         * @retval true  已校验并消费响应牌；有结算目标时已完成对其的杀结算。
         * @retval false 未响应或非法选择（幽灵引用/非杀的牌），不消耗牌。
         * @post 返回 true 时响应牌已弃置并发事件；返回 false 时状态不变。
         * @note 响应侧不受出牌阶段杀次数限制（次数是出牌阶段「本回合已用杀」的
         *         簿记，响应窗口不在出牌阶段簿记内）。结算目标由引擎固定（借刀
         *         的 B，打出时已以同一距离谓词校验），响应侧不复核目标，与真杀
         *         响应路径一致。转化来源的合法性由「武将技能 + 所选牌面」无歧义
         *         识别，回传的 PlayAction 无需携带转化标记。借刀响应事件
         *         语法：对目标结算=打出、仅消费=响应语义的弃置事件（展示为打出；
         *         真杀与虚拟杀同口径）。
         */
        bool respond_sha(
            GameContext &ctx, DecisionSource &ai,
            const std::string &entity, const std::string &victim,
            const ResponsePrompt &prompt);

        // ── 效果辅助 ────────────────────────────────────────────────────

        /** @brief 目标选牌集合：按 targets 顺序的 (owner, 选中的牌) 对。 */
        using TargetPicks = std::vector<std::pair<std::string, card::Card>>;

        /**
         * @brief 收集「对每个目标选一张牌」的决策（全部收集校验，不落子）。
         * @param[in] ctx      对局上下文。
         * @param[in] ai       决策源；逐目标询问选牌。
         * @param[in] player   效果使用者实体 id。
         * @param[in] targets  目标 id 列表（按此顺序返回 picks）。
         * @param[in] is_trick 该效果是否锦囊；false 不开无懈窗口。
         * @param[in] def      锦囊定义，透传给 resolve_nullification。
         * @return 按 targets 顺序的选牌集合。
         * @retval Ok(picks) 收集并校验通过（已跳过被无懈抵消的目标）。
         * @retval Err(EffectError::InvalidChoice) 某目标选择为空，或所选牌不在该
         *         目标任一区域。
         * @post 只读收集：不移动/弃置牌、不发牌域事件；被无懈抵消的目标不进
         *       picks。落子由调用方在收集校验成功后执行。
         * @note 无懈窗口逐目标单元素，与 `EffectInvocation::nullified` 同口径。
         */
        GameResult<TargetPicks> collect_target_picks(
            GameContext &ctx, DecisionSource &ai, const std::string &player,
            const std::vector<std::string> &targets, bool is_trick,
            const card::CardDef &def);

        // ── 效果分派（按 kind 的结算函数）────────────────────────────────

        namespace detail
        {
            /**
             * @brief 一次效果结算的引用束：引用 resolve_play 局部量，不持有所有权。
             * @warning 全为引用成员，生命周期仅覆盖本次结算调用；不得存储或跨
             *          调用使用。
             */
            struct EffectInvocation
            {
                GameContext &ctx;                       /**< 对局上下文。 */
                DecisionSource &ai;                     /**< 决策源。 */
                const std::string &player;              /**< 效果使用者 id。 */
                const card::Card &played;               /**< 打出的实体牌。 */
                const card::CardDef &def;               /**< 该牌定义。 */
                const card::CardEffect &eff;            /**< 该牌效果。 */
                const std::vector<std::string> &targets; /**< 目标 id 列表。 */
                bool is_trick;                          /**< 是否锦囊（开无懈窗口）。 */

                /**
                 * @brief 无懈窗口：仅锦囊开；窗口目标由调用点决定——多目标效果
                 *        逐目标传 `{t}`，单目标/组合效果传 `targets`。
                 * @param[in] window_targets 本窗覆盖的目标 id 列表。
                 * @return 本窗是否被无懈抵消（奇数张无懈）。
                 * @retval true  效果对本窗目标被抵消。
                 * @retval false 非锦囊或打出偶数张（含 0 张）无懈。
                 */
                bool nullified(const std::vector<std::string> &window_targets) const;
            };

            /**
             * @brief 群体效果目标重排：从使用者下家起、按座位序环绕。
             * @param[in] e 效果结算引用束（用其 ctx/player/targets）。
             * @return 只重排不改集合：targets 元素一个不少，顺序按座位环排列。
             * @note 与延时锦囊移送同口径 order_from(next(player))；scope=All 含
             *       使用者时使用者排在环绕末尾。不在存活座位环内的目标按原序追加
             *       （校验已保证 targets ⊆ 存活合法集，兜底不可达，仅防静默丢目标）。
             */
            std::vector<std::string> aoe_order_from_next(
                const EffectInvocation &e);

            /**
             * @brief 杀：逐目标按实体杀结算（伤害属性来自效果数据，共享一次酒加成）。
             * @param[in] e 效果结算引用束。
             * @return 始终 `Ok`（逐目标失败不冒泡；杀管线内部已按规则短路）。
             * @post 已消费本回合酒加成；每个目标经 `resolve_sha` 完成命中/响应。
             */
            GameResult<void> resolve_damage(const EffectInvocation &e);

            /**
             * @brief 酒（使用方法 I）：令本回合下一张使用的「杀」伤害 +1。
             * @param[in] e 效果结算引用束。
             * @return 始终 `Ok`。
             * @post `ctx.jiu_used = true` 且 `ctx.jiu_damage_owner = e.player`。
             * @note 使用限制（每回合一次）由 validate_play_action 闸门与回合流程
             *       保证；本函数只落回合内运行时状态，不持久化、不触碰存档。
             */
            GameResult<void> resolve_analeptic(const EffectInvocation &e);

            /**
             * @brief 群体伤害：从使用者下家起逐目标开单元素无懈窗口，未响应则受伤。
             * @param[in] e 效果结算引用束。
             * @return 始终 `Ok`。
             * @post 未被抵消且未响应且非藤甲免疫的目标经 `deal_damage` 受伤。
             */
            GameResult<void> resolve_aoe_damage(const EffectInvocation &e);

            /**
             * @brief 群体回复：从使用者下家起逐目标开单元素无懈窗口，未被抵消则回血。
             * @param[in] e 效果结算引用束。
             * @return 始终 `Ok`。
             * @post 未被抵消的目标经 `apply_heal` 回血（不超上限）。
             */
            GameResult<void> resolve_heal(const EffectInvocation &e);

            /**
             * @brief 摸牌：全量目标单窗，被无懈抵消则不摸牌。
             * @param[in] e 效果结算引用束。
             * @return 始终 `Ok`。
             * @post 未被抵消时使用者摸 `e.eff.count` 张。
             */
            GameResult<void> resolve_draw(const EffectInvocation &e);

            /**
             * @brief 过河拆桥：先收集校验全部目标选牌，再统一弃置（事务性）。
             * @param[in] e 效果结算引用束。
             * @return 结算结果。
             * @retval Ok  全部选牌已弃置。
             * @retval Err(EffectError::InvalidChoice) 收集阶段某目标选择非法，此时
             *         不弃置任何牌。
             * @post 成功时选中的牌进入弃牌堆并发事件；失败时不落子。
             */
            GameResult<void> resolve_discard_target(const EffectInvocation &e);

            /**
             * @brief 顺手牵羊：先收集校验全部目标选牌，再统一移入使用者手牌（事务性）。
             * @param[in] e 效果结算引用束。
             * @return 结算结果。
             * @retval Ok  选中的牌已移入使用者手牌。
             * @retval Err(EffectError::InvalidChoice) 收集或移除阶段失败，不落子。
             * @post 成功时牌经 `CardMoved` 事件移入使用者手牌；装备区失去时联动
             *       `apply_equip_lost`。
             */
            GameResult<void> resolve_steal(const EffectInvocation &e);

            /**
             * @brief 决斗：目标先出杀轮流，先不出者受伤；轮次耗尽平局。
             * @param[in] e 效果结算引用束（目标取首位）。
             * @return 始终 `Ok`。
             * @post 未被无懈抵消时，先不出杀者受 `e.eff.amount` 伤害；双方每轮都
             *       出杀则平局不造成伤害。
             */
            GameResult<void> resolve_duel(const EffectInvocation &e);

            /**
             * @brief 五谷丰登：亮牌按座位序各选一张，余牌弃置。
             * @param[in] e 效果结算引用束。
             * @return 结算结果。
             * @retval Ok  全部未被抵消目标的选牌已进手牌，余牌已弃置。
             * @retval Err(EffectError::InvalidChoice) 某未被抵消目标选择为空或不
             *         在亮牌中；已亮牌原样放回摸牌堆，打出的牌由 `resolve_play` 回滚。
             * @note 亮出等同存活人数的牌，未被抵消的目标按座位序各选一张。亮牌
             *       取自摸牌堆（堆空则弃牌堆洗回）；亮牌数不因逐目标无懈减少
             *       （卡面「亮出等同于角色数的牌」）。每个目标选牌前各开一个单元素
             *       无懈窗口，被抵消者不参与选牌。选择先按座位序只读收集并校验（须
             *       命中当前亮牌中的一张），再统一落子；五谷为强制选择。
             * @note 无牌可亮（摸牌堆与弃牌堆皆空）时提前结束收集，后续目标不再开窗，
             *       余牌为空。
             */
            GameResult<void> resolve_reveal_pick(const EffectInvocation &e);

            /**
             * @brief 借刀杀人：令持刀者出杀，不出则使用者掠夺其武器。
             * @param[in] e 效果结算引用束（targets = {持刀者, 受害者}）。
             * @return 始终 `Ok`。
             * @post 持刀者未出杀时，其武器经 `CardMoved` 事件移入使用者手牌。
             */
            GameResult<void> resolve_borrowed_sword(const EffectInvocation &e);

            /**
             * @brief 铁索连环：逐目标单元素无懈窗，未被抵消者翻转连环状态。
             * @param[in] e 效果结算引用束。
             * @return 始终 `Ok`。
             * @post 未被抵消目标经 `set_chained` 翻转横置状态。
             */
            GameResult<void> resolve_chain(const EffectInvocation &e);

            /**
             * @brief 火攻：目标展示一张手牌，使用者可弃一张同花色手牌造成火焰伤害。
             * @param[in] e 效果结算引用束（目标取首位）。
             * @return 始终 `Ok`；放弃或非法选择按「不伤害」处理，不报错。
             * @post 使用者弃牌成功时对目标造成火焰伤害（藤甲额外 +1）。
             * @note 无懈窗口在展示之前（单目标一窗）；目标结算时已无手牌即视为
             *       火攻失败（牌已弃，不展示不伤害）。展示由目标本人选，候选为其
             *       本人手牌；弃牌由使用者本人选，候选只含其同花色手牌。放弃/非法
             *       选择一律按「不伤害」处理，不抛 InvalidChoice，故无需回滚。
             * @note 火焰直伤经 deal_damage（indirect=false，可触发连环传导）；
             *       藤甲的火焰脆弱仅在杀管线内累加，此处对目标另按能力 +1。
             */
            GameResult<void> resolve_fire_attack(const EffectInvocation &e);

            /**
             * @brief 按 effect.kind 分派到对应结算函数。
             * @param[in] e 效果结算引用束。
             * @return 结算结果。
             * @retval Ok  已分派到对应结算函数（含部分效果的始终 Ok 语义）。
             * @retval Err(EffectError::UnsupportedKind) 该 kind 尚无结算分支。
             */
            GameResult<void> apply_effect(const EffectInvocation &e);
        }  // namespace detail

        // ── 结算入口 ────────────────────────────────────────────────────

        /**
         * @brief 结算「player 打出 played 牌，指定 targets」。
         * @param[in] ctx     对局上下文。
         * @param[in] ai      决策源。
         * @param[in] player  出牌实体 id。
         * @param[in] played  打出的牌（按值传入：结算会把该牌移出手牌）。
         * @param[in] targets 目标 id 列表。
         * @return 结算结果。
         * @retval Ok  效果已结算；装备返回 Ok 但不消耗（走 equip_card）。
         * @retval Err(EffectError::UnknownCard) 目录无此卡定义。
         * @retval Err(EffectError::UnsupportedKind) 无主动效果且非装备，或效果未实现。
         * @retval Err(EffectError::CardNotOwned) 打出的牌不在手牌中。
         * @retval Err(EffectError::NoTarget/OutOfRange/InvalidTarget) 目标预校验失败。
         * @retval Err(其他) 结算函数失败（如 InvalidChoice）。
         * @post 校验通过后先弃置打出的牌再应用效果；结算失败时从弃牌堆（必要时
         *       摸牌堆）取回，途中已消耗的响应牌不回退。
         * @note played 按值传入：结算过程会把该牌移出手牌，引用会失效。
         *       校验失败（未知卡/越范围/未实现/空目标）不消耗该牌；
         *       若途中摸牌堆空、弃牌堆洗回已将该牌并入摸牌堆，则改从摸牌堆取回
         *       （洗回已消耗随机流，不回退）。
         */
        GameResult<void> resolve_play(
            GameContext &ctx, DecisionSource &ai, const std::string &player,
            card::Card played, const std::vector<std::string> &targets);

        /**
         * @brief 结算「虚拟杀」（丈八蛇矛两张当杀 / 武圣单张红牌当杀）。
         * @param[in] ctx              对局上下文。
         * @param[in] ai               决策源。
         * @param[in] player           出牌实体 id。
         * @param[in] first_id         第一张来源手牌的 instance_id。
         * @param[in] second_id        第二张来源手牌的 instance_id；非空 = 两张当杀
         *                             （丈八蛇矛），为空 = 单张转化（武圣）。
         * @param[in] targets          目标 id 列表。
         * @param[in] validate_targets 是否复验目标（最终闸门，含方天画戟放宽）；
         *                             结算目标由引擎固定时（杀响应窗口）传 false
         *                             跳过——目标在打出时已以同一距离谓词校验，
         *                             与真杀响应路径一致。
         * @param[in] damage_bonus     命中伤害修正初值（主动使用虚拟杀时由回合入口
         *                             消费酒加成传入；响应/打出路径取默认 0）。
         * @return 结算结果。
         * @retval Ok  来源牌已消费，逐目标按虚拟杀结算。
         * @retval Err(EffectError::UnsupportedKind) 牌堆无「杀」定义，或来源不满足
         *         两张当杀/单张转化能力。
         * @retval Err(EffectError::CardNotOwned) 来源牌不在手牌或两牌重复。
         * @retval Err(EffectError::NoTarget/OutOfRange/InvalidTarget) 目标预校验失败。
         * @post 校验先于消费，失败不消耗牌；成功时每张来源牌各进弃牌堆并发打出
         *       事件。
         * @note 虚拟杀无花色：仁王盾黑杀判定不适用（经 resolve_sha 的 virtual
         *       标记短路）。
         */
        GameResult<void> resolve_virtual_sha(
            GameContext &ctx, DecisionSource &ai, const std::string &player,
            const std::string &first_id, const std::string &second_id,
            const std::vector<std::string> &targets, bool validate_targets = true,
            int damage_bonus = 0);
    }
}

#endif  // INCLUDE_TKW_GAME_RESOLVER_HPP