/**
 * @file   resolver.cpp
 * @brief  效果结算器的函数体定义。
 * @details 实现 `resolver.hpp` 声明的目标选牌收集、按 CardEffectKind 的效果分派、
 *          结算入口与杀响应窗口；结算校验先于消费，失败不消耗打出的牌。
 * @ingroup tkw_game_resolve
 */

#include "game/resolve/resolver.hpp"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace tkw
{
    namespace game
    {
        GameResult<TargetPicks> collect_target_picks(
            GameContext &ctx, DecisionSource &ai, const std::string &player,
            const std::vector<std::string> &targets, bool is_trick,
            const card::CardDef &def)
        {
            TargetPicks picks;
            for (const auto &t : targets)
            {
                // 无懈窗口逐目标单元素，与 EffectInvocation::nullified 同口径
                if (is_trick &&
                    CounterResolver(ctx, ai).resolve_nullification(
                        CounterWindow::Builder{}
                            .trick(&def)
                            .trick_user(player)
                            .targets({t})
                            .build()))
                    continue;
                const auto picked = ai.pick_card_from_target(
                    ctx, player, t, PickCardScope::HandEquipJudge);
                if (picked.is_none())
                    return GameResult<TargetPicks>::Err(EffectError::InvalidChoice);
                const auto chosen = StateOps(ctx).resolve_target_pick(t, picked.unwrap());
                if (chosen.is_none() ||
                    !ctx.cards->has_card(t, chosen.unwrap().instance_id))
                    return GameResult<TargetPicks>::Err(EffectError::InvalidChoice);
                picks.emplace_back(t, chosen.unwrap());
            }
            return GameResult<TargetPicks>::Ok(std::move(picks));
        }

        namespace detail
        {
            bool EffectInvocation::nullified(
                const std::vector<std::string> &window_targets) const
            {
                return is_trick &&
                       CounterResolver(ctx, ai).resolve_nullification(
                           CounterWindow::Builder{}
                               .trick(&def)
                               .trick_user(player)
                               .targets(window_targets)
                               .build());
            }

            std::vector<std::string> aoe_order_from_next(
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

            GameResult<void> resolve_damage(const EffectInvocation &e)
            {
                // 使用「杀」即消费本回合的酒加成：被闪/被防具无效也已使用，不再保留；
                // 方天多目标共享同一次消费（每个目标都 +1）
                const int jiu = StateOps(e.ctx).consume_jiu_sha_bonus(e.player);
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

            GameResult<void> resolve_analeptic(const EffectInvocation &e)
            {
                e.ctx.jiu_used = true;
                e.ctx.jiu_damage_owner = e.player;
                return GameResult<void>::Ok();
            }

            GameResult<void> resolve_aoe_damage(const EffectInvocation &e)
            {
                const auto ordered = aoe_order_from_next(e);
                for (const auto &t : ordered)
                {
                    // 藤甲：普通伤害的群体锦囊（南蛮/万箭）对该角色无效，
                    // 不进入响应窗口（与仁王盾「无效则不响应」同口径）
                    if (EquipQuery::has_ability(e.ctx, t, card::Ability::VineArmor) &&
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

            GameResult<void> resolve_heal(const EffectInvocation &e)
            {
                const auto ordered = aoe_order_from_next(e);
                for (const auto &t : ordered)
                {
                    if (e.nullified({t}))
                        continue;
                    StateOps(e.ctx).apply_heal(t, e.eff.amount);
                }
                return GameResult<void>::Ok();
            }

            GameResult<void> resolve_draw(const EffectInvocation &e)
            {
                if (e.nullified(e.targets))
                    return GameResult<void>::Ok();
                StateOps(e.ctx).apply_draw(e.player, e.eff.count);
                return GameResult<void>::Ok();
            }

            GameResult<void> resolve_discard_target(const EffectInvocation &e)
            {
                // 先收集并校验全部选择，再统一落子（事务性）
                auto picks_r = collect_target_picks(
                    e.ctx, e.ai, e.player, e.targets, e.is_trick, e.def);
                if (picks_r.is_err())
                    return GameResult<void>::Err(picks_r.unwrap_err());
                const auto picks = std::move(picks_r).unwrap();
                for (const auto &[owner, picked_card] : picks)
                {
                    if (StateOps(e.ctx).remove_any_and_discard(owner, picked_card.instance_id)
                            .is_none())
                        return GameResult<void>::Err(EffectError::InvalidChoice);
                }
                return GameResult<void>::Ok();
            }

            GameResult<void> resolve_steal(const EffectInvocation &e)
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
                    if (!StateOps(e.ctx).remove_card_from_zones(owner, picked_card.instance_id, removed, &from))
                        return GameResult<void>::Err(EffectError::InvalidChoice);
                    e.ctx.cards->add_to_hand(e.player, removed);
                    emit_card_moved(e.ctx, owner, e.player, removed, from, Zone::Hand);
                    if (from == Zone::Equip)
                        StateOps(e.ctx).apply_equip_lost(owner, removed);
                }
                return GameResult<void>::Ok();
            }

            GameResult<void> resolve_duel(const EffectInvocation &e)
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

            GameResult<void> resolve_reveal_pick(const EffectInvocation &e)
            {
                // 亮出等同存活人数的牌（摸牌堆空则弃牌堆洗回，口径同摸牌/判定）
                std::vector<card::Card> revealed;
                const int n = static_cast<int>(e.ctx.entities->size());
                for (int i = 0; i < n; ++i)
                {
                    auto c = StateOps(e.ctx).draw_with_refill();
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
                    StateOps(e.ctx).discard_and_emit("", c);
                return GameResult<void>::Ok();
            }

            GameResult<void> resolve_borrowed_sword(const EffectInvocation &e)
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

            GameResult<void> resolve_chain(const EffectInvocation &e)
            {
                for (const auto &t : e.targets)
                {
                    if (e.nullified({t}))
                        continue;
                    StateOps(e.ctx).set_chained(t, !StateQuery::is_chained(e.ctx, t));
                }
                return GameResult<void>::Ok();
            }

            GameResult<void> resolve_fire_attack(const EffectInvocation &e)
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
                    StateOps(e.ctx).remove_and_discard(e.player, chosen).is_none())
                    return GameResult<void>::Ok();

                // 火焰伤害：藤甲火焰脆弱 +1（杀管线之外的直伤在此补足）
                int amount = e.eff.amount;
                if (EquipQuery::has_ability(e.ctx, target, card::Ability::VineArmor))
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

            GameResult<void> apply_effect(const EffectInvocation &e)
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

        GameResult<void> resolve_play(
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

        GameResult<void> resolve_virtual_sha(
            GameContext &ctx, DecisionSource &ai, const std::string &player,
            const std::string &first_id, const std::string &second_id,
            const std::vector<std::string> &targets, bool validate_targets,
            int damage_bonus)
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

        bool respond_sha(
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
                if (!EquipQuery::has_ability(ctx, entity, card::Ability::TwoCardsAsSha) ||
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
                    HeroQuery::can_convert_card_to_sha(ctx, entity, *chosen_card,
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
                    StateOps(ctx).discard_and_emit(entity, std::move(removed).unwrap(),
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
