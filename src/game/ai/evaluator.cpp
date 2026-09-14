/**
 * @file   evaluator.cpp
 * @brief  牌价值评估函数的定义。
 * @ingroup tkw_game_ai
 */

#include "game/ai/evaluator.hpp"

namespace tkw
{
    namespace game
    {
        namespace ai
        {
            int card_value(const card::CardDef &def)
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
                case E::Chain:
                    return kCardValueUtility;
                case E::Damage:
                case E::DiscardTarget:
                case E::Duel:
                case E::Analeptic:
                case E::FireAttack:
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
