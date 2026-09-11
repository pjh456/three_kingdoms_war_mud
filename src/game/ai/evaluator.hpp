/**
 * @file evaluator.hpp
 * @brief AI 评估函数：牌价值（纯函数）。
 */

#ifndef INCLUDE_TKW_GAME_EVALUATOR_HPP
#define INCLUDE_TKW_GAME_EVALUATOR_HPP

#include "card/def.hpp"

namespace tkw
{
    namespace game
    {
        namespace ai
        {
            // 牌基础价值分档：数值越大越值得留/用；card_value 的返回只引用本处常量。
            inline constexpr int kCardValueCounter = 55;       /**< 无懈可击（counter 标记） */
            inline constexpr int kCardValueHeal = 50;          /**< 回复类（桃/桃园） */
            inline constexpr int kCardValueUtility = 45;       /**< 摸牌/顺/群体伤害/亮牌 */
            inline constexpr int kCardValueOffense = 40;       /**< 伤害/拆/决斗 */
            inline constexpr int kCardValueJink = 35;          /**< 闪 */
            inline constexpr int kCardValueBorrowedSword = 35; /**< 借刀杀人 */
            inline constexpr int kCardValueEquipment = 30;     /**< 装备 */
            inline constexpr int kCardValueLow = 10;           /**< 无主动效果/未知效果兜底 */

            /** @brief 单张牌的基础价值（越大越值得留/用）。 */
            inline int card_value(const card::CardDef &def)
            {
                using E = card::CardEffectKind;
                if (def.type == card::CardType::Equipment)
                    return kCardValueEquipment;
                if (def.effect.is_none())
                    return def.counter ? kCardValueCounter : kCardValueLow;  // 无懈可击价值高
                switch (def.effect.unwrap().kind)
                {
                case E::Heal:
                    return kCardValueHeal;
                case E::Draw:
                case E::Steal:
                case E::AoeDamage:
                case E::RevealPick:
                    return kCardValueUtility;
                case E::Damage:
                case E::DiscardTarget:
                case E::Duel:
                    return kCardValueOffense;
                case E::Jink:
                    return kCardValueJink;
                case E::BorrowedSword:
                    return kCardValueBorrowedSword;
                }
                return kCardValueLow;
            }
        }
    }
}

#endif  // INCLUDE_TKW_GAME_EVALUATOR_HPP
