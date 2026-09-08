/**
 * @file evaluator.hpp
 * @brief AI 评估函数：牌价值与威胁/集火评分（纯函数）。
 */

#ifndef INCLUDE_TKW_GAME_AI_EVALUATOR_HPP
#define INCLUDE_TKW_GAME_AI_EVALUATOR_HPP

#include <algorithm>

#include "card/def.hpp"
#include "game/ai/view.hpp"
#include "game/core/effect.hpp"

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

            /**
             * @brief 威胁分：越高越强/越该优先压制。
             * @note 体力 + 手牌 + 装备 + 武器 + 距离（越近越威胁）。
             */
            inline int threat_score(const EnemyView &e)
            {
                int s = e.hp * 2 + e.hand_size * 3 + e.equip_count * 4;
                if (e.has_weapon)
                    s += 6;
                s += std::max(0, 4 - e.distance) * 2;
                return s;
            }

            /** @brief 击杀优先：残血越少越优先（同血由调用方按列表序稳定）。 */
            inline int kill_priority(const EnemyView &e)
            {
                return (e.max_hp - e.hp) + 1;
            }
        }
    }
}

#endif  // INCLUDE_TKW_GAME_AI_EVALUATOR_HPP
