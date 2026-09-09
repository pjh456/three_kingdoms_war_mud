/**
 * @file legal.hpp
 * @brief 出牌阶段合法动作枚举：AI 与校验共用的单一事实源。
 * @note 产出的每个动作都应能被 execute_turn/resolve_play 接受；
 *       顺序 = 手牌顺序（确定性）。
 */

#ifndef INCLUDE_TKW_GAME_LEGAL_HPP
#define INCLUDE_TKW_GAME_LEGAL_HPP

#include <string>
#include <utility>
#include <vector>

#include "card/card.hpp"
#include "card/def.hpp"
#include "card/manager.hpp"
#include "game/core/context.hpp"
#include "game/core/decision.hpp"
#include "game/query/distance.hpp"
#include "game/query/judge.hpp"
#include "game/core/effect.hpp"
#include "game/query/equip.hpp"
#include "game/resolve/resolver.hpp"

namespace tkw
{
    namespace game
    {
        /** @brief 一个候选出牌动作。 */
        struct LegalAction
        {
            card::Card card;
            std::vector<std::string> targets; /**< 引擎认可的完整目标集合 */
        };

        /** @brief 实体任一区域是否有牌（拆/顺的目标需有牌可拿）。 */
        inline bool entity_has_any_card(
            const GameContext &ctx, const std::string &id)
        {
            return ctx.cards->hand_size(id) > 0 || ctx.cards->equip_size(id) > 0 ||
                   ctx.cards->judge_size(id) > 0;
        }

        /**
         * @brief 枚举 player 出牌阶段所有合法主动动作。
         * @param turn 提供本回合杀次数上限（已达上限则不再产出杀）。
         * @note 覆盖装备、延时锦囊、主动效果牌，以及借刀杀人的双目标特例。
         */
        inline std::vector<LegalAction> legal_actions(
            const GameContext &ctx, const std::string &player, const TurnContext &turn)
        {
            std::vector<LegalAction> out;

            for (const auto &c : ctx.cards->hand(player))
            {
                const auto def_opt = ctx.catalog->find(c.def_id);
                if (def_opt.is_none())
                    continue;
                const card::CardDef &def = *def_opt.unwrap();

                switch (classify_action(def))
                {
                case PlayClass::Equipment:
                    out.push_back(LegalAction{c, {}});
                    continue;

                case PlayClass::DelayedTrick:
                {
                    for (const auto &t : delayed_legal_targets(ctx, player, def))
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
                    // targets = {A(持武器者), B(A攻击范围内角色，可为A)}
                    for (const auto &e : *ctx.entities)
                    {
                        const std::string &holder = e->get_id();
                        if (holder == player)
                            continue;
                        if (!has_equip_slot(ctx, holder, card::EquipSlot::Weapon))
                            continue;
                        for (const auto &b : *ctx.entities)
                        {
                            const std::string &victim = b->get_id();
                            if (in_attack_range(ctx, holder, victim))
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
                if (scope == card::Scope::OneOther)
                {
                    // 单目标：每个通过校验的候选目标各产出一个动作
                    for (const auto &t : targets)
                    {
                        const auto ok = validate_play_action(
                            ctx, player, def, c, std::vector<std::string>{t}, turn)
                            .is_ok();
                        if (ok)
                            out.push_back(LegalAction{c, {t}});
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
            return out;
        }
    }
}

#endif  // INCLUDE_TKW_GAME_LEGAL_HPP
