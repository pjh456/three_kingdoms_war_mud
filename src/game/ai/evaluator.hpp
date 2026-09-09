/**
 * @file evaluator.hpp
 * @brief AI 评估函数：牌价值（纯函数）。
 */

#ifndef INCLUDE_TKW_GAME_AI_EVALUATOR_HPP
#define INCLUDE_TKW_GAME_AI_EVALUATOR_HPP

#include "card/def.hpp"

namespace tkw
{
    namespace game
    {
        namespace ai
        {
            /** @brief 单张牌的基础价值（越大越值得留/用）。 */
            inline int card_value(const card::CardDef &def)
            {
                using E = card::CardEffectKind;
                if (def.type == card::CardType::Equipment)
                    return 30;
                if (def.effect.is_none())
                    return def.counter ? 55 : 10;  // 无懈可击价值高
                switch (def.effect.unwrap().kind)
                {
                case E::Heal:
                    return 50;
                case E::Draw:
                case E::Steal:
                case E::AoeDamage:
                case E::RevealPick:
                    return 45;
                case E::Damage:
                case E::DiscardTarget:
                case E::Duel:
                    return 40;
                case E::Jink:
                    return 35;
                case E::BorrowedSword:
                    return 35;
                }
                return 10;
            }
        }
    }
}

#endif  // INCLUDE_TKW_GAME_AI_EVALUATOR_HPP
