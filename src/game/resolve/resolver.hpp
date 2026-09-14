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

        // 杀响应窗口入口（决斗/南蛮/借刀共用；定义见结算区末尾）
        inline bool respond_sha(
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
        inline GameResult<TargetPicks> collect_target_picks(
            GameContext &ctx, DecisionSource &ai, const std::string &player,
            const std::vector<std::string> &targets, bool is_trick,
            const card::CardDef &def)
        {
            TargetPicks picks;
            for (const auto &t : targets)
            {
                // 无懈窗口逐目标单元素，与 EffectInvocation::nullified 同口径
                if (is_trick && resolve_nullification(ctx, ai, def, player, {t}))
                    continue;
                const auto picked = ai.pick_card_from_target(
                    ctx, player, t, PickCardScope::HandEquipJudge);
                if (picked.is_none())
                    return GameResult<TargetPicks>::Err(EffectError::InvalidChoice);
                const auto chosen = resolve_target_pick(ctx, t, picked.unwrap());
                if (chosen.is_none() ||
                    !ctx.cards->has_card(t, chosen.unwrap().instance_id))
                    return GameResult<TargetPicks>::Err(EffectError::InvalidChoice);
                picks.emplace_back(t, chosen.unwrap());
            }
            return GameResult<TargetPicks>::Ok(std::move(picks));
        }

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
                bool nullified(const std::vector<std::string> &window_targets) const
                {
                    return is_trick &&
                           resolve_nullification(ctx, ai, def, player, window_targets);
                }
            };

            /**
             * @brief 群体效果目标重排：从使用者下家起、按座位序环绕。
             * @param[in] e 效果结算引用束（用其 ctx/player/targets）。
             * @return 只重排不改集合：targets 元素一个不少，顺序按座位环排列。
             * @note 与延时锦囊移送同口径 order_from(next(player))；scope=All 含
             *       使用者时使用者排在环绕末尾。不在存活座位环内的目标按原序追加
             *       （校验已保证 targets ⊆ 存活合法集，兜底不可达，仅防静默丢目标）。
             */
            inline std::vector<std::string> aoe_order_from_next(
                const EffectInvocation &e)
            {
                std::vector<std::string> ordered;
                ordered.reserve(e.targets.size());
                for (const auto &id :
                     e.ctx.entities->order_from(e.ctx.entities->next(e.player)))
                    if (std::find(e.targets.begin(), e.targets.end(), id) !=
                        e.targets.end())
                        ordered.push_back(id);

                // 兜底：不在座位环内的目标按原序追加（不丢目标）
                for (const auto &t : e.targets)
                    if (std::find(ordered.begin(), ordered.end(), t) ==
                        ordered.end())
                        ordered.push_back(t);
                return ordered;
            }

            /**
             * @brief 杀：逐目标按实体杀结算（伤害属性来自效果数据，共享一次酒加成）。
             * @param[in] e 效果结算引用束。
             * @return 始终 `Ok`（逐目标失败不冒泡；杀管线内部已按规则短路）。
             * @post 已消费本回合酒加成；每个目标经 `resolve_sha` 完成命中/响应。
             */
            inline GameResult<void> resolve_damage(const EffectInvocation &e)
            {
                // 使用「杀」即消费本回合的酒加成：被闪/被防具无效也已使用，不再保留；
                // 方天多目标共享同一次消费（每个目标都 +1）
                const int jiu = consume_jiu_sha_bonus(e.ctx, e.player);
                for (const auto &t : e.targets)
                    ShaResolver(e.ctx, e.ai)
                        .resolve_sha(
                            ShaRequest::Builder{}
                                .attacker(e.player)
                                .sha(e.played)
                                .target(t)
                                .damage_val(e.eff.amount)
                                .target_count(static_cast<int>(e.targets.size()))
                                .damage_type(e.eff.damage_type)
                                .damage_bonus(jiu)
                                .build());
                return GameResult<void>::Ok();
            }

            /**
             * @brief 酒（使用方法 I）：令本回合下一张使用的「杀」伤害 +1。
             * @param[in] e 效果结算引用束。
             * @return 始终 `Ok`。
             * @post `ctx.jiu_used = true` 且 `ctx.jiu_damage_owner = e.player`。
             * @note 使用限制（每回合一次）由 validate_play_action 闸门与回合流程
             *       保证；本函数只落回合内运行时状态，不持久化、不触碰存档。
             */
            inline GameResult<void> resolve_analeptic(const EffectInvocation &e)
            {
                e.ctx.jiu_used = true;
                e.ctx.jiu_damage_owner = e.player;
                return GameResult<void>::Ok();
            }

            /**
             * @brief 群体伤害：从使用者下家起逐目标开单元素无懈窗口，未响应则受伤。
             * @param[in] e 效果结算引用束。
             * @return 始终 `Ok`。
             * @post 未被抵消且未响应且非藤甲免疫的目标经 `deal_damage` 受伤。
             */
            inline GameResult<void> resolve_aoe_damage(const EffectInvocation &e)
            {
                const auto ordered = aoe_order_from_next(e);
                for (const auto &t : ordered)
                {
                    // 藤甲：普通伤害的群体锦囊（南蛮/万箭）对该角色无效，
                    // 不进入响应窗口（与仁王盾「无效则不响应」同口径）
                    if (has_ability(e.ctx, t, card::Ability::VineArmor) &&
                        e.eff.damage_type == card::DamageType::Normal)
                        continue;
                    if (e.nullified({t}))
                        continue;
                    bool responded = false;
                    if (e.eff.response.is_some())
                    {
                        const auto kind = e.eff.response.unwrap();
                        const ResponsePrompt prompt{
                            e.def.id, e.player, e.eff.amount};
                        responded = kind == card::ResponseKind::Sha
                            ? respond_sha(e.ctx, e.ai, t, "", prompt)
                            : request_jink(e.ctx, e.ai, t, prompt);
                    }
                    if (!responded)
                        CombatResolver(e.ctx, e.ai)
                            .deal_damage(
                                t, DamageSpec::Builder{}
                                       .source(e.player)
                                       .damage_val(e.eff.amount)
                                       .build());
                }
                return GameResult<void>::Ok();
            }

            /**
             * @brief 群体回复：从使用者下家起逐目标开单元素无懈窗口，未被抵消则回血。
             * @param[in] e 效果结算引用束。
             * @return 始终 `Ok`。
             * @post 未被抵消的目标经 `apply_heal` 回血（不超上限）。
             */
            inline GameResult<void> resolve_heal(const EffectInvocation &e)
            {
                const auto ordered = aoe_order_from_next(e);
                for (const auto &t : ordered)
                {
                    if (e.nullified({t}))
                        continue;
                    apply_heal(e.ctx, t, e.eff.amount);
                }
                return GameResult<void>::Ok();
            }

            /**
             * @brief 摸牌：全量目标单窗，被无懈抵消则不摸牌。
             * @param[in] e 效果结算引用束。
             * @return 始终 `Ok`。
             * @post 未被抵消时使用者摸 `e.eff.count` 张。
             */
            inline GameResult<void> resolve_draw(const EffectInvocation &e)
            {
                if (e.nullified(e.targets))
                    return GameResult<void>::Ok();
                apply_draw(e.ctx, e.player, e.eff.count);
                return GameResult<void>::Ok();
            }

            /**
             * @brief 过河拆桥：先收集校验全部目标选牌，再统一弃置（事务性）。
             * @param[in] e 效果结算引用束。
             * @return 结算结果。
             * @retval Ok  全部选牌已弃置。
             * @retval Err(EffectError::InvalidChoice) 收集阶段某目标选择非法，此时
             *         不弃置任何牌。
             * @post 成功时选中的牌进入弃牌堆并发事件；失败时不落子。
             */
            inline GameResult<void> resolve_discard_target(const EffectInvocation &e)
            {
                // 先收集并校验全部选择，再统一落子（事务性）
                auto picks_r = collect_target_picks(
                    e.ctx, e.ai, e.player, e.targets, e.is_trick, e.def);
                if (picks_r.is_err())
                    return GameResult<void>::Err(picks_r.unwrap_err());
                const auto picks = std::move(picks_r).unwrap();
                for (const auto &[owner, picked_card] : picks)
                {
                    if (remove_any_and_discard(e.ctx, owner, picked_card.instance_id)
                            .is_none())
                        return GameResult<void>::Err(EffectError::InvalidChoice);
                }
                return GameResult<void>::Ok();
            }

            /**
             * @brief 顺手牵羊：先收集校验全部目标选牌，再统一移入使用者手牌（事务性）。
             * @param[in] e 效果结算引用束。
             * @return 结算结果。
             * @retval Ok  选中的牌已移入使用者手牌。
             * @retval Err(EffectError::InvalidChoice) 收集或移除阶段失败，不落子。
             * @post 成功时牌经 `CardMoved` 事件移入使用者手牌；装备区失去时联动
             *       `apply_equip_lost`。
             */
            inline GameResult<void> resolve_steal(const EffectInvocation &e)
            {
                // 先收集并校验全部选择，再统一落子（事务性）
                auto picks_r = collect_target_picks(
                    e.ctx, e.ai, e.player, e.targets, e.is_trick, e.def);
                if (picks_r.is_err())
                    return GameResult<void>::Err(picks_r.unwrap_err());
                const auto picks = std::move(picks_r).unwrap();
                for (const auto &[owner, picked_card] : picks)
                {
                    card::Card removed;
                    Zone from = Zone::Limbo;
                    if (!remove_card_from_zones(
                            e.ctx, owner, picked_card.instance_id, removed, &from))
                        return GameResult<void>::Err(EffectError::InvalidChoice);
                    e.ctx.cards->add_to_hand(e.player, removed);
                    emit_card_moved(e.ctx, owner, e.player, removed, from, Zone::Hand);
                    if (from == Zone::Equip)
                        apply_equip_lost(e.ctx, owner, removed);
                }
                return GameResult<void>::Ok();
            }

            /**
             * @brief 决斗：目标先出杀轮流，先不出者受伤；轮次耗尽平局。
             * @param[in] e 效果结算引用束（目标取首位）。
             * @return 始终 `Ok`。
             * @post 未被无懈抵消时，先不出杀者受 `e.eff.amount` 伤害；双方每轮都
             *       出杀则平局不造成伤害。
             */
            inline GameResult<void> resolve_duel(const EffectInvocation &e)
            {
                if (e.nullified(e.targets))
                    return GameResult<void>::Ok();

                // 目标先开始，轮流打出杀；先不出的受对方 1 点伤害；
                // 轮次耗尽（双方每轮都出了杀）平局结算。
                std::string attacker = e.player;
                std::string defender = e.targets.front();
                for (int round = 0; round < rules_of(e.ctx).duel_rounds; ++round)
                {
                    if (!respond_sha(
                            e.ctx, e.ai, defender, "",
                            {e.def.id, e.player, e.eff.amount}))
                    {
                        CombatResolver(e.ctx, e.ai)
                            .deal_damage(
                                defender, DamageSpec::Builder{}
                                              .source(attacker)
                                              .damage_val(e.eff.amount)
                                              .build());
                        return GameResult<void>::Ok();
                    }
                    std::swap(attacker, defender);
                }
                // 轮次耗尽：双方每轮都出了杀、无人「先不出」→ 平局，不再造成伤害
                return GameResult<void>::Ok();
            }

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
            inline GameResult<void> resolve_reveal_pick(const EffectInvocation &e)
            {
                // 亮出等同存活人数的牌（摸牌堆空则弃牌堆洗回，口径同摸牌/判定）
                std::vector<card::Card> revealed;
                const int n = static_cast<int>(e.ctx.entities->size());
                for (int i = 0; i < n; ++i)
                {
                    auto c = draw_with_refill(e.ctx);
                    if (c.is_none())
                        break;
                    revealed.push_back(std::move(c).unwrap());
                }
                const std::vector<card::Card> revealed_order = revealed;

                // 按座位序（从使用者开始）收集选择并校验：每个目标先开单元素
                // 无懈窗口，被抵消者跳过不选牌；命中当前亮牌之一并从候选池移除，
                // 供后位玩家选择；不下子、不发事件
                std::vector<std::pair<std::string, card::Card>> picks;
                for (const auto &p : e.ctx.entities->order_from(e.player))
                {
                    if (revealed.empty())
                        break;
                    if (e.nullified({p}))
                        continue;
                    const auto picked =
                        e.ai.pick_from_revealed(e.ctx, p, revealed, RevealSource::Wugu);
                    std::size_t idx = revealed.size();
                    if (picked.is_some())
                        for (std::size_t k = 0; k < revealed.size(); ++k)
                            if (revealed[k].instance_id == picked.unwrap().instance_id)
                            {
                                idx = k;
                                break;
                            }
                    if (idx == revealed.size())
                    {
                        // 非法选择：按原亮牌序放回摸牌堆（含先行已收集的选择），
                        // 整体失败不分配也不弃置
                        for (auto it = revealed_order.rbegin();
                             it != revealed_order.rend(); ++it)
                            e.ctx.cards->add_to_draw(*it);
                        return GameResult<void>::Err(EffectError::InvalidChoice);
                    }
                    picks.emplace_back(p, revealed[idx]);
                    revealed.erase(revealed.begin() + std::ptrdiff_t(idx));
                }

                // 收集校验通过后统一落子：选中牌进手牌
                for (const auto &[p, chosen] : picks)
                {
                    e.ctx.cards->add_to_hand(p, chosen);
                    emit_card_moved(e.ctx, "", p, chosen, Zone::Limbo, Zone::Hand);
                }

                // 剩余置入弃牌堆
                for (const auto &c : revealed)
                    discard_and_emit(e.ctx, "", c);
                return GameResult<void>::Ok();
            }

            /**
             * @brief 借刀杀人：令持刀者出杀，不出则使用者掠夺其武器。
             * @param[in] e 效果结算引用束（targets = {持刀者, 受害者}）。
             * @return 始终 `Ok`。
             * @post 持刀者未出杀时，其武器经 `CardMoved` 事件移入使用者手牌。
             */
            inline GameResult<void> resolve_borrowed_sword(const EffectInvocation &e)
            {
                if (e.nullified(e.targets))
                    return GameResult<void>::Ok();
                const std::string &holder = e.targets[0];
                const std::string &victim = e.targets[1];

                if (respond_sha(
                        e.ctx, e.ai, holder, victim,
                        {e.def.id, e.player, 0}))
                    return GameResult<void>::Ok();

                // 未出杀：使用者获得 holder 的武器
                for (const auto &c : e.ctx.cards->equip(holder))
                {
                    const auto d = e.ctx.catalog->find(c.def_id);
                    if (d.is_some() && d.unwrap()->equip.is_some() &&
                        d.unwrap()->equip.unwrap().slot == card::EquipSlot::Weapon)
                    {
                        auto removed =
                            e.ctx.cards->remove_from_equip(holder, c.instance_id);
                        if (removed.is_some())
                        {
                            card::Card weapon = std::move(removed).unwrap();
                            e.ctx.cards->add_to_hand(e.player, weapon);
                            emit_card_moved(
                                e.ctx, holder, e.player, weapon, Zone::Equip, Zone::Hand);
                        }
                        break;
                    }
                }
                return GameResult<void>::Ok();
            }

            /**
             * @brief 铁索连环：逐目标单元素无懈窗，未被抵消者翻转连环状态。
             * @param[in] e 效果结算引用束。
             * @return 始终 `Ok`。
             * @post 未被抵消目标经 `set_chained` 翻转横置状态。
             */
            inline GameResult<void> resolve_chain(const EffectInvocation &e)
            {
                for (const auto &t : e.targets)
                {
                    if (e.nullified({t}))
                        continue;
                    set_chained(e.ctx, t, !is_chained(e.ctx, t));
                }
                return GameResult<void>::Ok();
            }

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
            inline GameResult<void> resolve_fire_attack(const EffectInvocation &e)
            {
                if (e.nullified(e.targets))
                    return GameResult<void>::Ok();
                const std::string &target = e.targets.front();

                // 结算时目标已无手牌：火攻失败（牌已弃），不展示不伤害
                const auto target_hand = e.ctx.cards->hand(target);
                if (target_hand.empty())
                    return GameResult<void>::Ok();

                // 目标本人展示一张手牌；非法/放弃回落首张（确定性兜底）
                card::Card revealed = target_hand.front();
                const auto shown = e.ai.pick_from_revealed(
                    e.ctx, target, target_hand, RevealSource::FireAttackReveal);
                if (shown.is_some())
                    for (const auto &c : target_hand)
                        if (c.instance_id == shown.unwrap().instance_id)
                        {
                            revealed = c;
                            break;
                        }

                // 使用者同花色手牌（可放弃）：无匹配即不伤害
                std::vector<card::Card> matching;
                for (const auto &c : e.ctx.cards->hand(e.player))
                    if (c.suit == revealed.suit)
                        matching.push_back(c);
                if (matching.empty())
                    return GameResult<void>::Ok();

                // 放弃/非法选择 = 不伤害（None 语义按来源区分）
                const auto discard = e.ai.pick_from_revealed(
                    e.ctx, e.player, matching, RevealSource::FireAttackDiscard);
                if (discard.is_none())
                    return GameResult<void>::Ok();
                std::string chosen;
                for (const auto &c : matching)
                    if (c.instance_id == discard.unwrap().instance_id)
                    {
                        chosen = c.instance_id;
                        break;
                    }
                if (chosen.empty() ||
                    remove_and_discard(e.ctx, e.player, chosen).is_none())
                    return GameResult<void>::Ok();

                // 火焰伤害：藤甲火焰脆弱 +1（杀管线之外的直伤在此补足）
                int amount = e.eff.amount;
                if (has_ability(e.ctx, target, card::Ability::VineArmor))
                    amount += 1;
                CombatResolver(e.ctx, e.ai)
                    .deal_damage(
                        target, DamageSpec::Builder{}
                                    .source(e.player)
                                    .damage_val(amount)
                                    .damage_type(e.eff.damage_type)
                                    .build());
                return GameResult<void>::Ok();
            }

            /**
             * @brief 按 effect.kind 分派到对应结算函数。
             * @param[in] e 效果结算引用束。
             * @return 结算结果。
             * @retval Ok  已分派到对应结算函数（含部分效果的始终 Ok 语义）。
             * @retval Err(EffectError::UnsupportedKind) 该 kind 尚无结算分支。
             */
            inline GameResult<void> apply_effect(const EffectInvocation &e)
            {
                switch (e.eff.kind)
                {
                case card::CardEffectKind::Damage:
                    return resolve_damage(e);
                case card::CardEffectKind::AoeDamage:
                    return resolve_aoe_damage(e);
                case card::CardEffectKind::Heal:
                    return resolve_heal(e);
                case card::CardEffectKind::Draw:
                    return resolve_draw(e);
                case card::CardEffectKind::DiscardTarget:
                    return resolve_discard_target(e);
                case card::CardEffectKind::Steal:
                    return resolve_steal(e);
                case card::CardEffectKind::Duel:
                    return resolve_duel(e);
                case card::CardEffectKind::RevealPick:
                    return resolve_reveal_pick(e);
                case card::CardEffectKind::BorrowedSword:
                    return resolve_borrowed_sword(e);
                case card::CardEffectKind::Analeptic:
                    return resolve_analeptic(e);
                case card::CardEffectKind::Chain:
                    return resolve_chain(e);
                case card::CardEffectKind::FireAttack:
                    return resolve_fire_attack(e);
                default:
                    return GameResult<void>::Err(EffectError::UnsupportedKind);
                }
            }
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
        inline GameResult<void> resolve_play(
            GameContext &ctx, DecisionSource &ai, const std::string &player,
            card::Card played, const std::vector<std::string> &targets)
        {
            const auto def_opt = ctx.catalog->find(played.def_id);
            if (def_opt.is_none())
                return GameResult<void>::Err(EffectError::UnknownCard);
            const card::CardDef &def = *def_opt.unwrap();
            if (def.effect.is_none())
            {
                // 装备由 equip_card 处理；其余无主动效果（如无懈）不可主动打出
                if (def.type == card::CardType::Equipment)
                    return GameResult<void>::Ok();
                return GameResult<void>::Err(EffectError::UnsupportedKind);
            }
            const card::CardEffect &eff = def.effect.unwrap();

            // 目标预校验（单一副本见 validate_effect_targets；本处为最终闸门）
            auto tr = validate_effect_targets(ctx, player, def, targets);
            if (tr.is_err())
                return tr;

            // 未实现的效果：不消耗打出的牌
            if (!is_settleable_kind(eff.kind))
                return GameResult<void>::Err(EffectError::UnsupportedKind);

            // 打出的牌必须在手牌中，否则拒绝（不消耗、不结算）
            auto played_removed = ctx.cards->remove_from_hand(player, played.instance_id);
            if (played_removed.is_none())
                return GameResult<void>::Err(EffectError::CardNotOwned);

            // 打出的牌先弃置（防止结算中被再次选中）
            ctx.cards->discard(std::move(played_removed).unwrap());
            emit_card_played(ctx, player, played);

            // 无懈可击只抵消锦囊牌；基本牌（杀/闪/桃）不可无懈
            // 窗口粒度 = 每个受影响目标一窗，窗内只携带该目标（卡面「对一名角色」）
            const bool is_trick = def.type == card::CardType::Trick;

            const detail::EffectInvocation invocation{
                ctx, ai, player, played, def, eff, targets, is_trick};
            const auto rr = detail::apply_effect(invocation);
            if (rr.is_err())
            {
                // 结算失败回滚：取回打出的牌（结算途中摸牌堆空洗回时，该牌可能
                // 已随弃牌堆并入摸牌堆）；结算中途已消耗的响应牌（如决斗中打出
                // 的杀）不回退
                auto back = ctx.cards->remove_from_discard(played.instance_id);
                if (back.is_none())
                    back = ctx.cards->remove_from_draw(played.instance_id);
                if (back.is_some())
                    ctx.cards->add_to_hand(player, std::move(back).unwrap());
            }
            return rr;
        }

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
        inline GameResult<void> resolve_virtual_sha(
            GameContext &ctx, DecisionSource &ai, const std::string &player,
            const std::string &first_id, const std::string &second_id,
            const std::vector<std::string> &targets, bool validate_targets = true,
            int damage_bonus = 0)
        {
            const auto sha_def = find_sha_def(ctx);
            if (sha_def.is_none())
                return GameResult<void>::Err(EffectError::UnsupportedKind);
            const card::CardDef &sha = *sha_def.unwrap();
            const card::CardEffect &eff = sha.effect.unwrap();

            const bool two_cards = !second_id.empty();
            const std::size_t cards_consumed = two_cards ? 2 : 1;

            // 目标预校验（最终闸门；方天放宽按消耗手牌张数判定）
            if (validate_targets)
            {
                auto tr = validate_effect_targets(ctx, player, sha, targets,
                                                  cards_consumed);
                if (tr.is_err())
                    return tr;
            }

            // 来源牌都在手牌中且不重复（校验先于消费，失败不消耗）
            bool have_first = false;
            bool have_second = false;
            for (const auto &c : ctx.cards->hand(player))
            {
                if (c.instance_id == first_id)
                    have_first = true;
                else if (two_cards && c.instance_id == second_id)
                    have_second = true;
            }
            if (!have_first ||
                (two_cards && (first_id == second_id || !have_second)))
                return GameResult<void>::Err(EffectError::CardNotOwned);

            // 来源牌先弃置（防止结算中被再次选中）
            auto removed = ctx.cards->remove_from_hand(player, first_id);
            if (removed.is_none())
                return GameResult<void>::Err(EffectError::CardNotOwned);
            card::Card first = std::move(removed).unwrap();
            ctx.cards->discard(first);
            emit_card_played(ctx, player, first);
            if (two_cards)
            {
                removed = ctx.cards->remove_from_hand(player, second_id);
                if (removed.is_none())
                    return GameResult<void>::Err(EffectError::CardNotOwned);
                card::Card second = std::move(removed).unwrap();
                ctx.cards->discard(second);
                emit_card_played(ctx, player, second);
            }

            // 逐目标虚拟杀结算（无实体牌：花色仅仁王盾黑杀判定消费，已短路）
            const card::Card virtual_sha;
            for (const auto &t : targets)
                ShaResolver(ctx, ai)
                    .resolve_sha(
                        ShaRequest::Builder{}
                            .attacker(player)
                            .sha(virtual_sha)
                            .target(t)
                            .damage_val(eff.amount)
                            .target_count(static_cast<int>(targets.size()))
                            .virtual_sha(true)
                            .damage_type(card::DamageType::Normal)
                            .damage_bonus(damage_bonus)
                            .build());
            return GameResult<void>::Ok();
        }

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
        inline bool respond_sha(
            GameContext &ctx, DecisionSource &ai,
            const std::string &entity, const std::string &victim,
            const ResponsePrompt &prompt)
        {
            if (!has_response_card(ctx, entity, card::ResponseKind::Sha))
                return false;
            const auto chosen =
                ai.play_response(ctx, entity, card::ResponseKind::Sha, prompt);
            if (chosen.is_none())
                return false;
            const auto &act = chosen.unwrap();

            if (!act.second_instance_id.empty())
            {
                // 两张当杀（丈八蛇矛）：先校验，非法选择按不响应、不消耗
                bool in_first = false;
                bool in_second = false;
                if (act.instance_id != act.second_instance_id)
                {
                    for (const auto &c : ctx.cards->hand(entity))
                    {
                        if (c.instance_id == act.instance_id)
                            in_first = true;
                        else if (c.instance_id == act.second_instance_id)
                            in_second = true;
                    }
                }
                // 能力/两牌在手闸；结算目标引擎固定，不复核（见 @note）
                if (!has_ability(ctx, entity, card::Ability::TwoCardsAsSha) ||
                    find_sha_def(ctx).is_none() || !in_first || !in_second)
                    return false;

                // 有结算目标：虚拟杀结算接管（消费+逐目标结算，跳过目标复验）
                if (!victim.empty())
                    return resolve_virtual_sha(
                               ctx, ai, entity, act.instance_id,
                               act.second_instance_id,
                               std::vector<std::string>{victim}, false)
                         .is_ok();

                // 无结算目标（决斗/南蛮）：仅消费两张
                auto removed = ctx.cards->remove_from_hand(entity, act.instance_id);
                if (removed.is_none())
                    return false;
                card::Card first = std::move(removed).unwrap();
                ctx.cards->discard(first);
                emit_card_played(ctx, entity, first);
                removed = ctx.cards->remove_from_hand(entity, act.second_instance_id);
                if (removed.is_none())
                    return false;
                card::Card second = std::move(removed).unwrap();
                ctx.cards->discard(second);
                emit_card_played(ctx, entity, second);
                return true;
            }

            // 武圣转化：所选红牌非真杀时按虚拟杀打出（真杀仍走下方原路径）
            const card::Card *chosen_card = nullptr;
            for (const auto &c : ctx.cards->hand(entity))
                if (c.instance_id == act.instance_id)
                {
                    chosen_card = &c;
                    break;
                }
            if (chosen_card != nullptr)
            {
                const auto cdef = ctx.catalog->find(chosen_card->def_id);
                const bool real_sha =
                    cdef.is_some() &&
                    is_response_def(*cdef.unwrap(), card::ResponseKind::Sha);
                if (cdef.is_some() && !real_sha &&
                    can_convert_card_to_sha(ctx, entity, *chosen_card,
                                            *cdef.unwrap()))
                {
                    // 有结算目标：虚拟杀接管（消费 + 逐目标结算，跳过目标复验）
                    if (!victim.empty())
                        return resolve_virtual_sha(
                                   ctx, ai, entity, act.instance_id, "",
                                   std::vector<std::string>{victim}, false)
                            .is_ok();

                    // 无结算目标（决斗/南蛮）：仅按响应语义消费并弃置
                    auto removed =
                        ctx.cards->remove_from_hand(entity, act.instance_id);
                    if (removed.is_none())
                        return false;
                    discard_and_emit(ctx, entity, std::move(removed).unwrap(),
                                     DiscardKind::Response);
                    return true;
                }
            }

            // 真杀：单牌消费
            auto removed = ctx.cards->remove_from_hand(entity, act.instance_id);
            if (removed.is_none())
                return false;
            card::Card card = std::move(removed).unwrap();
            const auto def = ctx.catalog->find(card.def_id);
            if (def.is_none() ||
                !is_response_def(*def.unwrap(), card::ResponseKind::Sha))
            {
                ctx.cards->add_to_hand(entity, std::move(card));
                return false;
            }
            ctx.cards->discard(card);
            if (!victim.empty())
            {
                // 对目标结算的响应计打出（与虚拟杀响应同口径）
                int dmg = rules_of(ctx).default_damage;
                card::DamageType dtype = card::DamageType::Normal;
                if (def.unwrap()->effect.is_some())
                {
                    dmg = def.unwrap()->effect.unwrap().amount;
                    dtype = def.unwrap()->effect.unwrap().damage_type;
                }
                emit_card_played(ctx, entity, card);
                ShaResolver(ctx, ai).resolve_sha(
                    ShaRequest::Builder{}
                        .attacker(entity)
                        .sha(card)
                        .target(victim)
                        .damage_val(dmg)
                        .damage_type(dtype)
                        .build());
            }
            else
                emit_card_discarded(ctx, entity, card, DiscardKind::Response);
            return true;
        }
    }
}

#endif  // INCLUDE_TKW_GAME_RESOLVER_HPP