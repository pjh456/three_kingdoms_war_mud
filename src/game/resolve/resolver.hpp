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
#include "game/resolve/combat.hpp"
#include "game/core/context.hpp"
#include "game/resolve/counter.hpp"
#include "game/core/decision.hpp"
#include "game/query/distance.hpp"
#include "game/query/judge.hpp"
#include "game/core/effect.hpp"
#include "game/query/equip.hpp"
#include "game/resolve/response.hpp"
#include "game/core/state.hpp"
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
            const std::string &entity, const std::string &victim);

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
                // 无懈窗口逐目标单元素，与 resolve_play 的 nullified 闭包同口径
                if (is_trick && resolve_nullification(ctx, ai, def, player, {t}))
                    continue;
                const auto picked = ai.pick_card_from_target(ctx, player, t);
                if (picked.is_none() ||
                    !ctx.cards->has_card(t, picked.unwrap().instance_id))
                    return GameResult<TargetPicks>::Err(EffectError::InvalidChoice);
                picks.emplace_back(t, picked.unwrap());
            }
            return GameResult<TargetPicks>::Ok(std::move(picks));
        }

        // ── 结算入口 ────────────────────────────────────────────────────

        /**
         * @brief 结算「player 打出 played 牌，指定 targets」。
         * @note played 按值传入：结算过程会把该牌移出手牌，引用会失效。
         *       校验失败（未知卡/越范围/未实现/空目标）不消耗该牌；
         *       校验通过后先把打出的牌弃置，再应用效果。
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
            const auto nullified = [&](const std::vector<std::string> &window_targets)
            {
                return is_trick &&
                       resolve_nullification(ctx, ai, def, player, window_targets);
            };

            const auto apply = [&]() -> GameResult<void>
            {
                switch (eff.kind)
                {
                case card::CardEffectKind::Damage:
                    for (const auto &t : targets)
                        resolve_sha(
                            ctx, ai, player, played, t, eff.amount,
                            static_cast<int>(targets.size()));
                    return GameResult<void>::Ok();

                case card::CardEffectKind::AoeDamage:
                    for (const auto &t : targets)
                    {
                        if (nullified({t}))
                            continue;
                        bool responded = false;
                        if (eff.response.is_some())
                        {
                            const auto kind = eff.response.unwrap();
                            responded = kind == card::ResponseKind::Sha
                                ? respond_sha(ctx, ai, t, "")
                                : request_response(ctx, ai, t, kind);
                        }
                        if (!responded)
                            deal_damage(ctx, ai, player, t, eff.amount);
                    }
                    return GameResult<void>::Ok();

                case card::CardEffectKind::Heal:
                    for (const auto &t : targets)
                    {
                        if (nullified({t}))
                            continue;
                        apply_heal(ctx, t, eff.amount);
                    }
                    return GameResult<void>::Ok();

                case card::CardEffectKind::Draw:
                    if (nullified(targets))
                        return GameResult<void>::Ok();
                    apply_draw(ctx, player, eff.count);
                    return GameResult<void>::Ok();

                case card::CardEffectKind::DiscardTarget:
                {
                    // 先收集并校验全部选择，再统一落子（事务性）
                    auto picks_r =
                        collect_target_picks(ctx, ai, player, targets, is_trick, def);
                    if (picks_r.is_err())
                        return GameResult<void>::Err(picks_r.unwrap_err());
                    const auto picks = std::move(picks_r).unwrap();
                    for (const auto &[owner, picked_card] : picks)
                    {
                        if (remove_any_and_discard(
                                ctx, owner, picked_card.instance_id)
                                .is_none())
                            return GameResult<void>::Err(EffectError::InvalidChoice);
                    }
                    return GameResult<void>::Ok();
                }

                case card::CardEffectKind::Steal:
                {
                    // 先收集并校验全部选择，再统一落子（事务性）
                    auto picks_r =
                        collect_target_picks(ctx, ai, player, targets, is_trick, def);
                    if (picks_r.is_err())
                        return GameResult<void>::Err(picks_r.unwrap_err());
                    const auto picks = std::move(picks_r).unwrap();
                    for (const auto &[owner, picked_card] : picks)
                    {
                        card::Card removed;
                        Zone from = Zone::Limbo;
                        if (!remove_card_from_zones(
                                ctx, owner, picked_card.instance_id, removed, &from))
                            return GameResult<void>::Err(EffectError::InvalidChoice);
                        ctx.cards->add_to_hand(player, removed);
                        emit_card_moved(ctx, owner, player, removed, from, Zone::Hand);
                    }
                    return GameResult<void>::Ok();
                }

                case card::CardEffectKind::Duel:
                    if (nullified(targets))
                        return GameResult<void>::Ok();
                    {
                        // 目标先开始，轮流打出杀；先不出的受对方 1 点伤害；
                        // 轮次耗尽（双方每轮都出了杀）平局结算。
                        std::string attacker = player;
                        std::string defender = targets.front();
                        for (int round = 0; round < rules_of(ctx).duel_rounds; ++round)
                        {
                            if (!respond_sha(ctx, ai, defender, ""))
                            {
                                deal_damage(ctx, ai, attacker, defender, eff.amount);
                                return GameResult<void>::Ok();
                            }
                            std::swap(attacker, defender);
                        }
                        // 轮次耗尽：双方每轮都出了杀、无人「先不出」→ 平局，不再造成伤害
                        return GameResult<void>::Ok();
                    }

                case card::CardEffectKind::RevealPick:
                {
                    if (nullified(targets))
                        return GameResult<void>::Ok();

                    // 亮出等同存活人数的牌（摸牌堆空则弃牌堆洗回，口径同摸牌/判定）
                    std::vector<card::Card> revealed;
                    const int n = static_cast<int>(ctx.entities->size());
                    for (int i = 0; i < n; ++i)
                    {
                        auto c = draw_with_refill(ctx);
                        if (c.is_none())
                            break;
                        revealed.push_back(std::move(c).unwrap());
                    }

                    // 按座位序（从使用者开始）依次选一张
                    for (const auto &p : ctx.entities->order_from(player))
                    {
                        if (revealed.empty())
                            break;
                        const auto picked = ai.pick_from_revealed(ctx, p, revealed);
                        std::size_t idx = 0;
                        if (picked.is_some())
                            for (std::size_t k = 0; k < revealed.size(); ++k)
                                if (revealed[k].instance_id ==
                                    picked.unwrap().instance_id)
                                {
                                    idx = k;
                                    break;
                                }
                        card::Card chosen = revealed[idx];
                        revealed.erase(revealed.begin() + std::ptrdiff_t(idx));
                        ctx.cards->add_to_hand(p, chosen);
                        emit_card_moved(ctx, "", p, chosen, Zone::Limbo, Zone::Hand);
                    }
                    // 剩余置入弃牌堆
                    for (const auto &c : revealed)
                        discard_and_emit(ctx, "", c);
                    return GameResult<void>::Ok();
                }

                case card::CardEffectKind::BorrowedSword:
                {
                    if (nullified(targets))
                        return GameResult<void>::Ok();
                    const std::string &holder = targets[0];
                    const std::string &victim = targets[1];

                    if (respond_sha(ctx, ai, holder, victim))
                        return GameResult<void>::Ok();

                    // 未出杀：使用者获得 holder 的武器
                    for (const auto &c : ctx.cards->equip(holder))
                    {
                        const auto d = ctx.catalog->find(c.def_id);
                        if (d.is_some() && d.unwrap()->equip.is_some() &&
                            d.unwrap()->equip.unwrap().slot ==
                                card::EquipSlot::Weapon)
                        {
                            auto removed =
                                ctx.cards->remove_from_equip(holder, c.instance_id);
                            if (removed.is_some())
                            {
                                card::Card weapon = std::move(removed).unwrap();
                                ctx.cards->add_to_hand(player, weapon);
                                emit_card_moved(
                                    ctx, holder, player, weapon, Zone::Equip,
                                    Zone::Hand);
                            }
                            break;
                        }
                    }
                    return GameResult<void>::Ok();
                }

                default:
                    return GameResult<void>::Err(EffectError::UnsupportedKind);
                }
            };

            const auto rr = apply();
            if (rr.is_err())
            {
                // 结算失败回滚：仅取回打出的牌；结算中途已消耗的响应牌
                // （如决斗中打出的杀）不回退
                auto back = ctx.cards->remove_from_discard(played.instance_id);
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
         * @note 目标校验（最终闸门，含方天画戟放宽）与两牌在手检查先于消费，
         *       失败不消耗牌；消费后两张各进弃牌堆并各发打出事件（防结算中
         *       被再选），再逐目标按虚拟杀结算。虚拟杀无花色：仁王盾黑杀
         *       判定不适用（经 resolve_sha 的 virtual 标记短路）。
         */
        inline GameResult<void> resolve_virtual_sha(
            GameContext &ctx, DecisionSource &ai, const std::string &player,
            const std::string &first_id, const std::string &second_id,
            const std::vector<std::string> &targets, bool validate_targets = true)
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
                            static_cast<int>(targets.size()), true);
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
         *         响应路径一致。借刀响应事件语法：对目标结算=打出、仅消费=弃置
         *         （真杀与虚拟杀同口径）。
         */
        inline bool respond_sha(
            GameContext &ctx, DecisionSource &ai,
            const std::string &entity, const std::string &victim)
        {
            if (!has_response_card(ctx, entity, card::ResponseKind::Sha))
                return false;
            const auto chosen = ai.play_response(ctx, entity, card::ResponseKind::Sha);
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

            // 真杀：单牌消费（与既有响应窗口行为一致）
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
                if (def.unwrap()->effect.is_some())
                    dmg = def.unwrap()->effect.unwrap().amount;
                emit_card_played(ctx, entity, card);
                resolve_sha(ctx, ai, entity, card, victim, dmg);
            }
            else
                emit_card_discarded(ctx, entity, card);
            return true;
        }
    }
}

#endif  // INCLUDE_TKW_GAME_RESOLVER_HPP