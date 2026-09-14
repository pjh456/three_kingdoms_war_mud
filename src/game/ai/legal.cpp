/**
 * @file   legal.cpp
 * @brief  出牌阶段合法动作枚举与两张当杀 pair 的定义。
 * @ingroup tkw_game_ai
 */

#include "game/ai/legal.hpp"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace tkw
{
    namespace game
    {
        std::vector<std::pair<card::Card, card::Card>> two_cards_as_sha_pairs(
            const ReadOnlyContext &ctx, const std::string &player)
        {
            std::vector<std::pair<card::Card, card::Card>> out;
            if (!EquipQuery::has_ability(ctx, player, card::Ability::TwoCardsAsSha))
                return out;
            const auto &hand = ctx.cards->hand(player);
            if (hand.size() < 2 || find_sha_def(ctx).is_none())
                return out;
            for (const auto &c : hand)
            {
                const auto d = ctx.catalog->find(c.def_id);
                if (d.is_some() && d.unwrap()->effect.is_some() &&
                    is_sha_kind(d.unwrap()->effect.unwrap().kind))
                    return out;  // 手牌有真杀：不产出虚拟杀
            }
            for (std::size_t i = 0; i + 1 < hand.size(); ++i)
                for (std::size_t j = i + 1; j < hand.size(); ++j)
                    out.emplace_back(hand[i], hand[j]);
            return out;
        }

        std::vector<LegalAction> legal_actions(
            const ReadOnlyContext &ctx, const std::string &player, const TurnContext &turn)
        {
            std::vector<LegalAction> out;
            // 重铸候选单独收集，统一追加在全部正常动作之后：分组时正常动作
            // 先入组，AI 的目标选择不会落到空目标的重铸动作上（不主动重铸）
            std::vector<LegalAction> recasts;

            for (const auto &c : ctx.cards->hand(player))
            {
                const auto def_opt = ctx.catalog->find(c.def_id);
                if (def_opt.is_none())
                    continue;
                const card::CardDef &def = *def_opt.unwrap();

                if (def.recast)
                    recasts.push_back(LegalAction{c, {}, "", true});

                switch (classify_action(def))
                {
                case PlayClass::Equipment:
                    out.push_back(LegalAction{c, {}});
                    continue;

                case PlayClass::DelayedTrick:
                {
                    for (const auto &t : JudgeQuery::delayed_legal_targets(ctx, player, def))
                        out.push_back(LegalAction{c, {t}});
                    continue;
                }

                case PlayClass::Active:
                case PlayClass::None:
                    break;
                }

                if (def.effect.is_none())
                    continue;
                const auto kind = def.effect.unwrap().kind;
                if (!is_settleable_kind(kind))
                    continue;

                if (kind == card::CardEffectKind::BorrowedSword)
                {
                    // targets = {A(持武器者), B(A攻击范围内另一名角色)}
                    const auto view = ctx.entities->const_view();
                    for (const auto *e : view)
                    {
                        const std::string &holder = e->get_id();
                        if (holder == player)
                            continue;
                        if (!EquipQuery::has_equip_slot(ctx, holder, card::EquipSlot::Weapon))
                            continue;
                        for (const auto *b : view)
                        {
                            const std::string &victim = b->get_id();
                            if (victim != holder &&
                                DistanceQuery::in_attack_range(ctx, holder, victim))
                                out.push_back(LegalAction{c, {holder, victim}});
                        }
                    }
                    continue;
                }

                auto targets = valid_targets(ctx, player, def);
                if (effect_traits(kind).target_card)
                {
                    std::vector<std::string> with_cards;
                    for (const auto &t : targets)
                        if (entity_has_any_card(ctx, t))
                            with_cards.push_back(t);
                    targets = std::move(with_cards);
                }
                if (targets.empty())
                    continue;

                const auto scope = def.effect.unwrap().scope.unwrap_or(card::Scope::Self);
                if (scope == card::Scope::OneOther || scope == card::Scope::AnyOne)
                {
                    // 单目标：每个通过校验的候选目标各产出一个动作
                    for (const auto &t : targets)
                    {
                        const auto ok = validate_play_action(
                            ctx, player, def, c, std::vector<std::string>{t}, turn)
                            .is_ok();
                        if (!ok)
                            continue;
                        out.push_back(LegalAction{c, {t}});

                        // 方天画戟：杀是最后一张手牌时，对同一原目标再产出一个
                        // 多目标动作（原目标 + 至多 2 名其他在范围内角色，
                        // 按实体序确定性截断，避免组合爆炸）
                        if (kind == card::CardEffectKind::Damage &&
                            EquipQuery::sha_multi_target(ctx, player))
                        {
                            std::vector<std::string> combo{t};
                            for (const auto &u : targets)
                            {
                                if (u != t)
                                    combo.push_back(u);
                                if (combo.size() >=
                                    static_cast<std::size_t>(
                                        rules_of(ctx).sha_multi_target_max))
                                    break;
                            }
                            if (combo.size() > 1 &&
                                validate_play_action(
                                    ctx, player, def, c, combo, turn).is_ok())
                                out.push_back(LegalAction{c, std::move(combo)});
                        }
                    }
                }
                else if (scope == card::Scope::OneOrTwo)
                {
                    // 单目标动作在前，双目标组合（i<j）在后；均过统一校验，
                    // 确定性顺序 = valid_targets 序，组合上界 C(n,2)
                    for (const auto &t : targets)
                        if (validate_play_action(
                                ctx, player, def, c, std::vector<std::string>{t}, turn)
                                .is_ok())
                            out.push_back(LegalAction{c, {t}});

                    for (std::size_t i = 0; i < targets.size(); ++i)
                        for (std::size_t j = i + 1; j < targets.size(); ++j)
                        {
                            std::vector<std::string> combo{targets[i], targets[j]};
                            if (validate_play_action(ctx, player, def, c, combo, turn)
                                    .is_ok())
                                out.push_back(LegalAction{c, std::move(combo)});
                        }
                }
                else
                {
                    const auto ok =
                        validate_play_action(ctx, player, def, c, targets, turn).is_ok();
                    if (ok)
                        out.push_back(LegalAction{c, std::move(targets)});
                }
            }

            // 丈八蛇矛：手牌无真杀时，两张手牌当一张杀（pair × 目标枚举，
            // 确定性手牌序；杀次数/目标合法性经 validate_virtual_sha 过滤）
            const auto pairs = two_cards_as_sha_pairs(ctx, player);
            if (!pairs.empty())
            {
                const auto sha_def = find_sha_def(ctx);
                const auto targets = valid_targets(ctx, player, *sha_def.unwrap());
                const bool multi = EquipQuery::sha_multi_target(ctx, player, 2);
                for (const auto &[first, second] : pairs)
                    for (const auto &t : targets)
                    {
                        if (!validate_virtual_sha(
                                ctx, player, first.instance_id, second.instance_id,
                                std::vector<std::string>{t}, turn)
                                .is_ok())
                            continue;
                        out.push_back(LegalAction{first, {t}, second.instance_id});

                        // 方天画戟：pair 为最后两张手牌时再产出多目标
                        // 动作（原目标 + 至多 2 名其他在范围内角色）
                        if (multi)
                        {
                            std::vector<std::string> combo{t};
                            for (const auto &u : targets)
                            {
                                if (u != t)
                                    combo.push_back(u);
                                if (combo.size() >=
                                    static_cast<std::size_t>(
                                        rules_of(ctx).sha_multi_target_max))
                                    break;
                            }
                            if (combo.size() > 1 &&
                                validate_virtual_sha(
                                    ctx, player, first.instance_id,
                                    second.instance_id, combo, turn)
                                    .is_ok())
                                out.push_back(LegalAction{
                                    first, std::move(combo), second.instance_id});
                        }
                    }
            }

            // 单张转化当杀（武圣红牌 / 龙胆闪，真杀已有普通动作不重复；
            // 杀次数/目标合法性经 validate_virtual_sha 过滤）
            const auto conversion_cards = HeroQuery::sha_conversion_cards(ctx, player);
            const auto sha_def = find_sha_def(ctx);
            if (!conversion_cards.empty() && sha_def.is_some())
            {
                const auto targets = valid_targets(ctx, player, *sha_def.unwrap());
                const bool multi = EquipQuery::sha_multi_target(ctx, player);
                for (const auto &c : conversion_cards)
                    for (const auto &t : targets)
                    {
                        if (!validate_virtual_sha(
                                ctx, player, c.instance_id, "",
                                std::vector<std::string>{t}, turn)
                                .is_ok())
                            continue;
                        out.push_back(LegalAction{c, {t}, "", false, true});

                        // 方天画戟：该红牌为最后一张手牌时再产出多目标
                        // 动作（原目标 + 至多 2 名其他在范围内角色）
                        if (multi)
                        {
                            std::vector<std::string> combo{t};
                            for (const auto &u : targets)
                            {
                                if (u != t)
                                    combo.push_back(u);
                                if (combo.size() >=
                                    static_cast<std::size_t>(
                                        rules_of(ctx).sha_multi_target_max))
                                    break;
                            }
                            if (combo.size() > 1 &&
                                validate_virtual_sha(
                                    ctx, player, c.instance_id, "", combo, turn)
                                    .is_ok())
                                out.push_back(LegalAction{
                                    c, std::move(combo), "", false, true});
                        }
                    }
            }

            out.insert(out.end(), recasts.begin(), recasts.end());
            return out;
        }
    }
}
