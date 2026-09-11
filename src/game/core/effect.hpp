/**
 * @file effect.hpp
 * @brief 效果类别与装备能力的属性表及卡牌响应分类谓词：CardEffectKind 的「能否结算 /
 *        可否主动打出 / 是否杀 / 是否需选目标牌」、Ability 的实现状态集中一处，
 *        消除散落的 switch。
 * @note 新增效果/能力只需在对应表加一行（效果另需补 resolve_play 结算分支）。
 */

#ifndef INCLUDE_TKW_GAME_EFFECT_HPP
#define INCLUDE_TKW_GAME_EFFECT_HPP

#include <cstdint>

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
            case E::BorrowedSword:
                return {true, true, false, false};
            }
            return {};
        }

        /** @brief 单个装备能力的静态属性。 */
        struct AbilityTraits
        {
            bool implemented = false; /**< 引擎是否已实现该能力结算 */
        };

        /** @brief 装备能力 → 属性。未知值一律取默认（未实现）。 */
        inline constexpr AbilityTraits ability_traits(card::Ability a)
        {
            using A = card::Ability;
            switch (a)
            {
            case A::NoShaLimit:
            case A::IgnoreArmor:
            case A::Cixiong:
            case A::ExtraShaAfterJink:
            case A::DiscardTwoForceDamage:
            case A::MultiTargetSha:
            case A::TwoCardsAsSha:
            case A::DiscardHorseOnDamage:
            case A::DamageAsDiscard:
            case A::JudgementJink:
            case A::BlackShaImmune:
                return {true};
            }
            return {};
        }

        /** @brief 装备能力引擎尚未实现（牌堆审计用）。 */
        inline constexpr bool is_unimplemented_ability(card::Ability a)
        {
            return !ability_traits(a).implemented;
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

        /** @brief 该效果是否为「杀」。 */
        inline constexpr bool is_sha_kind(card::CardEffectKind k)
        {
            return effect_traits(k).sha;
        }

        /** @brief 该定义是否为「延时锦囊」（锦囊、有判定描述、无主动效果）。 */
        inline bool is_delayed_trick(const card::CardDef &def)
        {
            return def.type == card::CardType::Trick && def.effect.is_none() &&
                   def.judge.is_some();
        }

        /** @brief 该定义是否可作为指定响应牌（杀=effect.kind==Damage，闪==Jink）。 */
        inline bool is_response_def(const card::CardDef &def, card::ResponseKind kind)
        {
            if (def.effect.is_none())
                return false;
            const auto k = def.effect.unwrap().kind;
            switch (kind)
            {
            case card::ResponseKind::Sha:
                return is_sha_kind(k);
            case card::ResponseKind::Jink:
                return k == card::CardEffectKind::Jink;
            }
            return false;
        }

        /** @brief 该定义是否可作濒死救场牌（数据标记 rescue，不再认 id）。 */
        inline bool is_rescue_def(const card::CardDef &def)
        {
            return def.rescue;
        }

        /** @brief 该定义是否可作无懈响应牌（数据标记 counter，不再认 id）。 */
        inline bool is_counter_def(const card::CardDef &def)
        {
            return def.counter;
        }

        /** @brief 出牌阶段的打出路径分类（回合流程/动作枚举共用同一分派）。 */
        enum class PlayClass : std::uint8_t
        {
            Equipment,    /**< 装备牌（走 equip_card，忽略目标） */
            DelayedTrick, /**< 延时锦囊（置入判定区） */
            Active,       /**< 主动效果（走 resolve_play） */
            None,        /**< 无主动效果（出牌阶段不可打出） */
        };

        /**
         * @brief 按卡牌定义分类出牌阶段的打出路径。
         * @note 纯静态分类（只看定义）：先装备、再延时锦囊、后有无主动效果；
         *       「杀」是状态规则（依赖回合上下文），保持正交谓词 is_sha_kind，
         *       不作第 5 个分类值。
         */
        inline PlayClass classify_action(const card::CardDef &def)
        {
            if (def.type == card::CardType::Equipment)
                return PlayClass::Equipment;
            if (is_delayed_trick(def))
                return PlayClass::DelayedTrick;
            if (def.effect.is_some())
                return PlayClass::Active;
            return PlayClass::None;
        }
    }
}

#endif  // INCLUDE_TKW_GAME_EFFECT_HPP
