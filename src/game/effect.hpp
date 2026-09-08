/**
 * @file effect.hpp
 * @brief 效果类别属性表：每个 CardEffectKind 的「能否结算 / 可否主动打出 /
 *        是否杀 / 是否需选目标牌」集中一处，消除散落的 switch。
 * @note 新增效果只需在此表加一行，并补 resolve_play 的结算分支。
 */

#ifndef INCLUDE_TKW_GAME_EFFECT_HPP
#define INCLUDE_TKW_GAME_EFFECT_HPP

#include "card/def.hpp"

namespace tkw
{
    namespace game
    {
        /** @brief 单个效果类别的静态属性。 */
        struct EffectTraits
        {
            bool implemented = false; /**< 引擎是否已实现结算 */
            bool active = false;      /**< 是否可在出牌阶段主动打出 */
            bool sha = false;         /**< 是否属于「杀」（次数限制/响应） */
            bool target_card = false; /**< 是否需从目标区域选牌（拆/顺） */
        };

        /** @brief 效果类别 → 属性。未知值一律取默认（未实现/不可主动）。 */
        inline constexpr EffectTraits effect_traits(card::CardEffectKind k)
        {
            using E = card::CardEffectKind;
            switch (k)
            {
            case E::Damage:
                return {true, true, true, false};
            case E::Jink:
                return {true, false, false, false};
            case E::Heal:
            case E::Draw:
            case E::AoeDamage:
            case E::Duel:
                return {true, true, false, false};
            case E::DiscardTarget:
            case E::Steal:
                return {true, true, false, true};
            case E::RevealPick:
                return {true, true, false, false};
            case E::BorrowedSword:
                return {false, true, false, false};
            }
            return {};
        }

        /** @brief 可主动打出且引擎能结算（resolve_play 接受）。 */
        inline constexpr bool is_settleable_kind(card::CardEffectKind k)
        {
            const auto t = effect_traits(k);
            return t.implemented && t.active;
        }

        /** @brief 本应可主动打出但引擎尚未实现（牌堆审计用）。 */
        inline constexpr bool is_unimplemented_active_kind(card::CardEffectKind k)
        {
            const auto t = effect_traits(k);
            return t.active && !t.implemented;
        }

        /** @brief 贪心 AI 可直接尝试的主动效果（杀单独走距离选目标）。 */
        inline constexpr bool is_ai_active_kind(card::CardEffectKind k)
        {
            const auto t = effect_traits(k);
            return t.implemented && t.active && !t.sha;
        }

        /** @brief 该效果是否为「杀」。 */
        inline constexpr bool is_sha_kind(card::CardEffectKind k)
        {
            return effect_traits(k).sha;
        }
    }
}

#endif  // INCLUDE_TKW_GAME_EFFECT_HPP
