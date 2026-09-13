/**
 * @file resolver.hpp
 * @brief 效果结算器：按 CardEffectKind 分派，把「打出的牌」作用于对局状态。
 * @note 只实现核心结算（杀/闪/桃/无中生有/过拆/顺牵/南蛮/万箭/决斗/桃园/
 *       五谷/借刀）；无懈/延时等返回 UnsupportedKind，作为明确未接缝；
 *       装备走 equip_card，不经本文件效果结算。
 *       响应牌消费与效果落子在本模块（单一写者）；只读的目标/距离校验见
 *       validate.hpp，出牌动作枚举见 ai/legal.hpp，玩家主观选择经
 *       DecisionSource 询问。
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
         * @param is_trick 该效果是否锦囊；false 不开无懈窗口。
         * @param def 锦囊定义，透传给 resolve_nullification。
         * @return Ok(picks) 按 targets 顺序（已跳过被无懈抵消的目标）；
         *         InvalidChoice = 某目标选择为空，或所选牌不在该目标任一区域。
         * @note 只读收集：不移动/弃置牌、不发牌域事件；被无懈抵消的目标不进
         *       picks。落子由调用方在收集校验成功后执行。
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
            /** @brief 一次效果结算的引用束：引用 resolve_play 局部量，不持有所有权。 */
            struct EffectInvocation
            {
                GameContext &ctx;
                DecisionSource &ai;
                const std::string &player;
                const card::Card &played;
                const card::CardDef &def;
                const card::CardEffect &eff;
                const std::vector<std::string> &targets;
                bool is_trick;

                /** @brief 无懈窗口：仅锦囊开；窗口目标由调用点决定——多目标效果
                 *         逐目标传 `{t}`，单目标/组合效果传 `targets`。 */
                bool nullified(const std::vector<std::string> &window_targets) const
                {
                    return is_trick &&
                           resolve_nullification(ctx, ai, def, player, window_targets);
                }
            };

            /**
             * @brief 群体效果目标重排：从使用者下家起、按座位序环绕。
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

            /** @brief 杀：逐目标按实体杀结算（伤害属性来自效果数据，共享一次酒加成）。 */
            inline GameResult<void> resolve_damage(const EffectInvocation &e)
            {
                // 使用「杀」即消费本回合的酒加成：被闪/被防具无效也已使用，不再保留；
                // 方天多目标共享同一次消费（每个目标都 +1）
                const int jiu = consume_jiu_sha_bonus(e.ctx, e.player);
                for (const auto &t : e.targets)
                    resolve_sha(
                        e.ctx, e.ai, e.player, e.played, t, e.eff.amount,
                        static_cast<int>(e.targets.size()), false,
                        e.eff.damage_type, jiu);
                return GameResult<void>::Ok();
            }

            /**
             * @brief 酒（使用方法 I）：令本回合下一张使用的「杀」伤害 +1。
             * @note 使用限制（每回合一次）由 validate_play_action 闸门与回合流程
             *       保证；本函数只落回合内运行时状态，不持久化、不触碰存档。
             */
            inline GameResult<void> resolve_analeptic(const EffectInvocation &e)
            {
                e.ctx.jiu_used = true;
                e.ctx.jiu_damage_owner = e.player;
                return GameResult<void>::Ok();
            }

            /** @brief 群体伤害：从使用者下家起逐目标开单元素无懈窗口，未响应则受伤。 */
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
                        deal_damage(e.ctx, e.ai, e.player, t, e.eff.amount);
                }
                return GameResult<void>::Ok();
            }

            /** @brief 群体回复：从使用者下家起逐目标开单元素无懈窗口，未被抵消则回血。 */
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

            /** @brief 摸牌：全量目标单窗，被无懈抵消则既摸。 */
            inline GameResult<void> resolve_draw(const EffectInvocation &e)
            {
                if (e.nullified(e.targets))
                    return GameResult<void>::Ok();
                apply_draw(e.ctx, e.player, e.eff.count);
                return GameResult<void>::Ok();
            }

            /** @brief 过河拆桥：先收集校验全部目标选牌，再统一弃置（事务性）。 */
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

            /** @brief 顺手牵羊：先收集校验全部目标选牌，再统一移入使用者手牌（事务性）。 */
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

            /** @brief 决斗：目标先出杀轮流，先不出者受伤；轮次耗尽平局。 */
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
                        deal_damage(e.ctx, e.ai, attacker, defender, e.eff.amount);
                        return GameResult<void>::Ok();
                    }
                    std::swap(attacker, defender);
                }
                // 轮次耗尽：双方每轮都出了杀、无人「先不出」→ 平局，不再造成伤害
                return GameResult<void>::Ok();
            }

            /**
             * @brief 五谷丰登：亮出等同存活人数的牌，未被抵消的目标按座位序各选一张，余牌弃置。
             * @note 亮牌取自摸牌堆（堆空则弃牌堆洗回）；亮牌数不因逐目标无懈减少
             *       （卡面「亮出等同于角色数的牌」）。每个目标选牌前各开一个单元素
             *       无懈窗口，被抵消者不参与选牌。选择先按座位序只读收集并校验（须
             *       命中当前亮牌中的一张），再统一落子；任一未被抵消的目标选择为空
             *       或不在亮牌中即 InvalidChoice 整体失败（五谷为强制选择），已亮的
             *       牌原样放回摸牌堆，打出的牌由 resolve_play 回滚。
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

            /** @brief 借刀杀人：令持刀者出杀，不出则使用者掠夺其武器。 */
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

            /** @brief 按 effect.kind 分派到对应结算函数；未实现返回 UnsupportedKind。 */
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
                default:
                    return GameResult<void>::Err(EffectError::UnsupportedKind);
                }
            }
        }  // namespace detail

        // ── 结算入口 ────────────────────────────────────────────────────

        /**
         * @brief 结算「player 打出 played 牌，指定 targets」。
         * @note played 按值传入：结算过程会把该牌移出手牌，引用会失效。
         *       校验失败（未知卡/越范围/未实现/空目标）不消耗该牌；
         *       校验通过后先把打出的牌弃置，再应用效果；结算失败时从弃牌堆
         *       取回，若途中摸牌堆空、弃牌堆洗回已将该牌并入摸牌堆，则改从
         *       摸牌堆取回（洗回已消耗随机流，不回退）。
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
         * @brief 结算「两张手牌当一张杀」（丈八蛇矛）。
         * @param first_id/second_id 两张手牌的 instance_id。
         * @param validate_targets 是否复验目标（最终闸门，含方天画戟放宽）；
         *        结算目标由引擎固定时（杀响应窗口）传 false 跳过——目标在
         *        打出时已以同一距离谓词校验，与真杀响应路径一致。
         * @param damage_bonus 命中伤害修正初值（主动使用丈八虚拟杀时由回合入口
         *        消费酒加成传入；响应/打出路径取默认 0）。
         * @note 目标校验（最终闸门，含方天画戟放宽）与两牌在手检查先于消费，
         *       失败不消耗牌；消费后两张各进弃牌堆并各发打出事件（防结算中
         *       被再选），再逐目标按虚拟杀结算。虚拟杀无花色：仁王盾黑杀
         *       判定不适用（经 resolve_sha 的 virtual 标记短路）。
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

            // 目标预校验（最终闸门；方天放宽按消耗两张手牌判定）
            if (validate_targets)
            {
                auto tr = validate_effect_targets(ctx, player, sha, targets, 2);
                if (tr.is_err())
                    return tr;
            }

            // 两张牌都在手牌中（校验先于消费，失败不消耗）
            bool have_first = false;
            bool have_second = false;
            for (const auto &c : ctx.cards->hand(player))
            {
                if (c.instance_id == first_id)
                    have_first = true;
                else if (c.instance_id == second_id)
                    have_second = true;
            }
            if (first_id == second_id || !have_first || !have_second)
                return GameResult<void>::Err(EffectError::CardNotOwned);

            // 两张牌先弃置（防止结算中被再次选中）
            auto removed = ctx.cards->remove_from_hand(player, first_id);
            if (removed.is_none())
                return GameResult<void>::Err(EffectError::CardNotOwned);
            card::Card first = std::move(removed).unwrap();
            ctx.cards->discard(first);
            emit_card_played(ctx, player, first);
            removed = ctx.cards->remove_from_hand(player, second_id);
            if (removed.is_none())
                return GameResult<void>::Err(EffectError::CardNotOwned);
            card::Card second = std::move(removed).unwrap();
            ctx.cards->discard(second);
            emit_card_played(ctx, player, second);

            // 逐目标虚拟杀结算（无实体牌：花色仅仁王盾黑杀判定消费，已短路）
            const card::Card virtual_sha;
            for (const auto &t : targets)
                resolve_sha(ctx, ai, player, virtual_sha, t, eff.amount,
                            static_cast<int>(targets.size()), true,
                            card::DamageType::Normal, damage_bonus);
            return GameResult<void>::Ok();
        }

        /**
         * @brief 开杀响应窗口并消费响应杀：响应者打出一张真杀，或（装备两张当杀
         *        能力时）打出两张手牌当杀。消费在本函数内完成（弃置+事件）；
         *        给出结算目标（借刀的 B）时再按杀对其结算（真杀带花色、虚拟杀
         *        无花色），否则仅消费（决斗/南蛮无结算目标）。
         * @return 是否发生了有效杀响应；非法选择（幽灵引用/非杀的牌）按不响应
         *         处理，不消耗牌。
         * @note 响应侧不受出牌阶段杀次数限制（次数是出牌阶段「本回合已用杀」的
         *         簿记，响应窗口不在出牌阶段簿记内）。结算目标由引擎固定（借刀
         *         的 B，打出时已以同一距离谓词校验），响应侧不复核目标，与真杀
         *         响应路径一致。借刀响应事件语法：对目标结算=打出、仅消费=响应
         *         语义的弃置事件（展示为打出；真杀与虚拟杀同口径）。
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
                resolve_sha(ctx, ai, entity, card, victim, dmg, 1, false, dtype);
            }
            else
                emit_card_discarded(ctx, entity, card, DiscardKind::Response);
            return true;
        }
    }
}

#endif  // INCLUDE_TKW_GAME_RESOLVER_HPP