/**
 * @file   validate.cpp
 * @brief  出牌动作只读校验的函数体定义。
 * @details 实现 `validate.hpp` 声明的目标选择与合法性预检；校验只读无副作用，
 *          不消费牌、不发事件。
 * @ingroup tkw_game_resolve
 */

#include "game/resolve/validate.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace tkw
{
    namespace game
    {
        Option<const card::CardDef *> find_sha_def(const ReadOnlyContext &ctx)
        {
            for (const auto &def : *ctx.catalog)
                if (def.effect.is_some() && is_sha_kind(def.effect.unwrap().kind))
                    return Option<const card::CardDef *>::Some(&def);
            return Option<const card::CardDef *>::None();
        }

        std::vector<std::string> valid_targets(
            const ReadOnlyContext &ctx, const std::string &player, const card::CardDef &def)
        {
            std::vector<std::string> out;
            if (def.effect.is_none())
                return out;
            const auto &eff = def.effect.unwrap();
            const auto scope = eff.scope.unwrap_or(card::Scope::Self);

            auto all_others = [&]()
            {
                for (const auto *e : ctx.entities->const_view())
                    if (e->get_id() != player)
                        out.push_back(e->get_id());
            };

            switch (scope)
            {
            case card::Scope::Self:
                // 桃：满体力时不可选自己（出牌阶段不能空放）；实体已移除时
                // 保持旧行为。濒死救场走 combat 的独立入口，不经本合法集
                if (eff.kind == card::CardEffectKind::Heal)
                {
                    const auto self = ctx.entities->find(player);
                    if (self.is_some() &&
                        self.unwrap()->get_hp() >=
                            self.unwrap()->get_hp_bar().get_max())
                        break;
                }
                out.push_back(player);
                break;
            case card::Scope::All:
                for (const auto *e : ctx.entities->const_view())
                    out.push_back(e->get_id());
                break;
            case card::Scope::OneOrTwo:
                // 1~2 名角色（含使用者），无距离限制
                for (const auto *e : ctx.entities->const_view())
                    out.push_back(e->get_id());
                break;
            case card::Scope::AnyOne:
                // 任意一名角色（含使用者），无距离限制；火攻要求目标有手牌
                for (const auto *e : ctx.entities->const_view())
                    if (ctx.cards->hand_size(e->get_id()) > 0)
                        out.push_back(e->get_id());
                break;
            case card::Scope::AllOthers:
                all_others();
                break;
            case card::Scope::OneOther:
                all_others();
                if (eff.kind == card::CardEffectKind::Damage)
                {
                    out.erase(
                        std::remove_if(out.begin(), out.end(),
                                       [&](const std::string &t)
                                       { return !DistanceQuery::in_attack_range(
                                             ctx, player, t); }),
                        out.end());
                }
                else if (eff.kind == card::CardEffectKind::Steal &&
                         !HeroQuery::ignores_trick_distance(ctx, player))
                {
                    out.erase(
                        std::remove_if(out.begin(), out.end(),
                                       [&](const std::string &t)
                                       { return !DistanceQuery::distance_le(
                                             ctx, player, t, eff.range); }),
                        out.end());
                }
                break;
            }
            return out;
        }

        GameResult<void> validate_effect_targets(
            const ReadOnlyContext &ctx, const std::string &player,
            const card::CardDef &def, const std::vector<std::string> &targets,
            std::size_t cards_consumed)
        {
            const card::CardEffect &eff = def.effect.unwrap();

            // 预校验：目标非空 + 距离
            if (targets.empty())
                return GameResult<void>::Err(EffectError::NoTarget);
            if (eff.kind == card::CardEffectKind::Damage)
            {
                for (const auto &t : targets)
                    if (!DistanceQuery::in_attack_range(ctx, player, t))
                        return GameResult<void>::Err(EffectError::OutOfRange);
            }
            else if (eff.kind == card::CardEffectKind::Steal &&
                     !HeroQuery::ignores_trick_distance(ctx, player))
            {
                for (const auto &t : targets)
                    if (!DistanceQuery::distance_le(ctx, player, t, eff.range))
                        return GameResult<void>::Err(EffectError::OutOfRange);
            }

            // 目标合法性：数量须符合 scope，且每个目标都必须在合法集合内
            if (eff.kind == card::CardEffectKind::BorrowedSword)
            {
                // 借刀杀人：targets = {A(持武器者), B(A攻击范围内另一名角色)}
                if (targets.size() != 2)
                    return GameResult<void>::Err(EffectError::InvalidTarget);
                const std::string &holder = targets[0];
                const std::string &victim = targets[1];
                if (holder == player || holder == victim ||
                    ctx.entities->find(holder).is_none() ||
                    ctx.entities->find(victim).is_none())
                    return GameResult<void>::Err(EffectError::InvalidTarget);
                if (!EquipQuery::has_equip_slot(ctx, holder, card::EquipSlot::Weapon))
                    return GameResult<void>::Err(EffectError::InvalidTarget);
                if (!DistanceQuery::in_attack_range(ctx, holder, victim))
                    return GameResult<void>::Err(EffectError::OutOfRange);
            }
            else
            {
                const auto scope = eff.scope.unwrap_or(card::Scope::Self);
                const auto legal = valid_targets(ctx, player, def);
                const auto in_legal = [&](const std::string &t)
                {
                    return std::find(legal.begin(), legal.end(), t) != legal.end();
                };
                bool target_ok = true;
                switch (scope)
                {
                case card::Scope::Self:
                case card::Scope::OneOther:
                case card::Scope::AnyOne:
                {
                    // 方天画戟：杀消耗完手中全部牌时共可指定至多 3 个目标
                    // （唯一目标 + 额外至多 2 名，卡面）；成员合法性由下方统一检查
                    const bool multi_sha =
                        eff.kind == card::CardEffectKind::Damage &&
                        EquipQuery::sha_multi_target(ctx, player, cards_consumed);
                    const std::size_t multi_max = static_cast<std::size_t>(
                        rules_of(ctx).sha_multi_target_max);

                    // 无重复：原集合长度须等于去重后长度，否则 {b,b}/{b,b,b}
                    // 这类重复目标会在结算端按原集合逐目标结算而重复造成伤害
                    std::vector<std::string> uniq = targets;
                    std::sort(uniq.begin(), uniq.end());
                    uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());
                    target_ok = targets.size() == uniq.size() &&
                                targets.size() <= (multi_sha ? multi_max : 1);
                    break;
                }
                case card::Scope::All:
                case card::Scope::AllOthers:
                    // 集合相等：All/AllOthers 须覆盖合法集合一次且仅一次。去重后
                    // 数量须等于合法集，且原集合长度须等于去重后长度，否则
                    // {b,c,d,b} 这类全覆盖 + 多余重复会重复结算
                    {
                        std::vector<std::string> uniq = targets;
                        std::sort(uniq.begin(), uniq.end());
                        uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());
                        target_ok = targets.size() == uniq.size() &&
                                    uniq.size() == legal.size();
                    }
                    break;
                case card::Scope::OneOrTwo:
                    // 一至两名：去重后仍须为 1~2 个，重复目标会重复翻转/结算
                    {
                        std::vector<std::string> uniq = targets;
                        std::sort(uniq.begin(), uniq.end());
                        uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());
                        target_ok = targets.size() == uniq.size() &&
                                    uniq.size() >= 1 && uniq.size() <= 2;
                    }
                    break;
                }
                if (target_ok)
                {
                    for (const auto &t : targets)
                        if (!in_legal(t))
                        {
                            target_ok = false;
                            break;
                        }
                }
                if (!target_ok)
                    return GameResult<void>::Err(EffectError::InvalidTarget);
            }
            return GameResult<void>::Ok();
        }

        GameResult<void> validate_play_action(
            const ReadOnlyContext &ctx, const std::string &player,
            const card::CardDef &def, const card::Card &card,
            const std::vector<std::string> &targets, const TurnContext &turn)
        {
            // 打出的牌必须在手牌中
            bool in_hand = false;
            for (const auto &c : ctx.cards->hand(player))
                if (c.instance_id == card.instance_id)
                {
                    in_hand = true;
                    break;
                }
            if (!in_hand)
                return GameResult<void>::Err(EffectError::CardNotOwned);

            // 重铸：弃置此牌并摸一张，空目标即重铸动作；正常出牌（带目标）仍
            // 走下方 scope 校验。旁路在分类之前，故杀上限/酒限次不约束重铸
            if (def.recast && targets.empty())
                return GameResult<void>::Ok();

            switch (classify_action(def))
            {
            case PlayClass::Equipment:
                return GameResult<void>::Ok();  // 装备走 equip_card，忽略目标

            case PlayClass::DelayedTrick:
                if (targets.size() != 1)
                    return GameResult<void>::Err(EffectError::InvalidTarget);
                if (!JudgeQuery::is_delayed_scope_target(ctx, player, def, targets.front()))
                    return GameResult<void>::Err(EffectError::InvalidTarget);
                if (JudgeQuery::has_same_delayed(ctx, targets.front(), def.id))
                    return GameResult<void>::Err(EffectError::DelayedDuplicate);
                return GameResult<void>::Ok();

            case PlayClass::Active:
                if (is_sha_kind(def.effect.unwrap().kind) &&
                    turn.sha_played >= turn.sha_limit)
                    return GameResult<void>::Err(EffectError::ShaLimitExceeded);
                if (def.effect.unwrap().kind == card::CardEffectKind::Analeptic &&
                    turn.analeptic_used)
                    return GameResult<void>::Err(
                        EffectError::AnalepticLimitExceeded);
                break;

            case PlayClass::None:
                return GameResult<void>::Err(EffectError::UnsupportedKind);
            }

            // 主动效果：目标校验后判可实现性
            auto tr = validate_effect_targets(ctx, player, def, targets);
            if (tr.is_err())
                return tr;
            if (!is_settleable_kind(def.effect.unwrap().kind))
                return GameResult<void>::Err(EffectError::UnsupportedKind);
            return GameResult<void>::Ok();
        }

        GameResult<void> validate_virtual_sha(
            const ReadOnlyContext &ctx, const std::string &player,
            const std::string &first_id, const std::string &second_id,
            const std::vector<std::string> &targets, const TurnContext &turn)
        {
            const bool two_cards = !second_id.empty();

            // 来源牌须为不同牌且都在手牌中（单张路径只查第一张）
            if (two_cards && first_id == second_id)
                return GameResult<void>::Err(EffectError::CardNotOwned);
            bool have_first = false;
            bool have_second = !two_cards;
            card::Card first_card;
            for (const auto &c : ctx.cards->hand(player))
            {
                if (c.instance_id == first_id)
                {
                    have_first = true;
                    first_card = c;
                }
                else if (c.instance_id == second_id)
                    have_second = true;
            }
            if (!have_first || !have_second)
                return GameResult<void>::Err(EffectError::CardNotOwned);

            const auto sha_def = find_sha_def(ctx);
            if (sha_def.is_none())
                return GameResult<void>::Err(EffectError::UnsupportedKind);

            // 来源闸：两张 = 丈八蛇矛能力；单张 = 武圣且首牌为红色
            std::size_t cards_consumed = 2;
            if (two_cards)
            {
                if (!EquipQuery::has_ability(ctx, player, card::Ability::TwoCardsAsSha))
                    return GameResult<void>::Err(EffectError::UnsupportedKind);
            }
            else
            {
                const auto first_def = ctx.catalog->find(first_card.def_id);
                if (first_def.is_none() ||
                    !HeroQuery::can_convert_card_to_sha(ctx, player, first_card,
                                             *first_def.unwrap()))
                    return GameResult<void>::Err(EffectError::UnsupportedKind);
                cards_consumed = 1;
            }

            // 虚拟杀按一张杀计次数
            if (turn.sha_played >= turn.sha_limit)
                return GameResult<void>::Err(EffectError::ShaLimitExceeded);

            return validate_effect_targets(ctx, player, *sha_def.unwrap(), targets,
                                           cards_consumed);
        }
    }
}
