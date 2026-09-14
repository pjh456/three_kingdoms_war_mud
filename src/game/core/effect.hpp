/**
 * @file effect.hpp
 * @brief 效果类别与装备能力的静态属性表及卡牌响应分类谓词。
 * @details 集中 `CardEffectKind` 的「能否结算 / 可否主动打出 / 是否杀 / 是否
 *          需选目标牌」、`Ability` 与武将技能的实现状态，消除散落的 switch。
 * @note 新增效果/能力只需在对应表加一行（效果另需补 `resolve_play` 结算分支）。
 * @ingroup tkw_game_core
 */

#ifndef INCLUDE_TKW_GAME_EFFECT_HPP
#define INCLUDE_TKW_GAME_EFFECT_HPP

#include <cstdint>

#include "card/def.hpp"
#include "hero/def.hpp"

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

        /**
         * @brief  效果类别 → 属性。
         * @param[in] k 效果类别。
         * @return 该类别的静态属性；未知值取默认（未实现/不可主动）。
         */
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
            case E::Analeptic:
            case E::Chain:
            case E::FireAttack:
                return {true, true, false, false};
            }
            return {};
        }

        /** @brief 单个装备能力的静态属性。 */
        struct AbilityTraits
        {
            bool implemented = false; /**< 引擎是否已实现该能力结算 */
        };

        /**
         * @brief  装备能力 → 属性。
         * @param[in] a 装备能力。
         * @return 该能力的静态属性；未知值取默认（未实现）。
         */
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
            case A::VineArmor:
            case A::GudingBlade:
            case A::SilverLion:
            case A::FireShaConvert:
                return {true};
            }
            return {};
        }

        /**
         * @brief  装备能力引擎尚未实现（牌堆审计用）。
         * @param[in] a 装备能力。
         * @return 未实现时为 true。
         */
        inline constexpr bool is_unimplemented_ability(card::Ability a)
        {
            return !ability_traits(a).implemented;
        }

        /** @brief 单个武将技能的静态属性。 */
        struct HeroSkillTraits
        {
            bool implemented = false; /**< 引擎是否已实现该技能结算 */
        };

        /**
         * @brief  武将技能 → 属性。
         * @param[in] s 武将技能。
         * @return 该技能的静态属性；未知值取默认（未实现）。
         */
        inline constexpr HeroSkillTraits hero_skill_traits(hero::HeroSkill s)
        {
            using H = hero::HeroSkill;
            switch (s)
            {
            case H::PaoXiao:
                return {true};
            case H::WuSheng:
                return {true};
            case H::YingZi:
                return {true};
            case H::FanKui:
                return {true};
            case H::MaShu:
                return {true};
            case H::QiCai:
                return {true};
            case H::LongDan:
                return {true};
            case H::QingGuo:
                return {true};
            }
            return {};
        }

        /**
         * @brief  武将技能引擎尚未实现（武将审计用）。
         * @param[in] s 武将技能。
         * @return 未实现时为 true。
         */
        inline constexpr bool is_unimplemented_skill(hero::HeroSkill s)
        {
            return !hero_skill_traits(s).implemented;
        }

        /**
         * @brief  可主动打出且引擎能结算（`resolve_play` 接受）。
         * @param[in] k 效果类别。
         * @return 已实现且可主动打出时为 true。
         */
        inline constexpr bool is_settleable_kind(card::CardEffectKind k)
        {
            const auto t = effect_traits(k);
            return t.implemented && t.active;
        }

        /**
         * @brief  本应可主动打出但引擎尚未实现（牌堆审计用）。
         * @param[in] k 效果类别。
         * @return 可主动打出但未实现时为 true。
         */
        inline constexpr bool is_unimplemented_active_kind(card::CardEffectKind k)
        {
            const auto t = effect_traits(k);
            return t.active && !t.implemented;
        }

        /**
         * @brief  该效果是否为「杀」。
         * @param[in] k 效果类别。
         * @return 属于「杀」时为 true。
         */
        inline constexpr bool is_sha_kind(card::CardEffectKind k)
        {
            return effect_traits(k).sha;
        }

        /**
         * @brief  该定义是否为「延时锦囊」。
         * @param[in] def 卡牌定义。
         * @return 锦囊、有判定描述且无主动效果时为 true。
         */
        inline bool is_delayed_trick(const card::CardDef &def)
        {
            return def.type == card::CardType::Trick && def.effect.is_none() &&
                   def.judge.is_some();
        }

        /**
         * @brief  该定义是否可作为指定响应牌。
         * @param[in] def  卡牌定义。
         * @param[in] kind 响应牌类别（杀 = effect.kind == Damage，闪 = Jink）。
         * @return 可作为该响应牌时为 true；无主动效果时为 false。
         */
        bool is_response_def(const card::CardDef &def, card::ResponseKind kind);

        /**
         * @brief  该定义是否可作濒死救场牌。
         * @param[in] def 卡牌定义。
         * @return 数据标记 `rescue` 为 true 时成立（不再认 id）。
         */
        inline bool is_rescue_def(const card::CardDef &def)
        {
            return def.rescue;
        }

        /**
         * @brief  该定义是否仅能救自己。
         * @param[in] def 卡牌定义。
         * @return 数据标记 `self_rescue` 为 true 时成立（如酒）。
         */
        inline bool is_self_rescue_def(const card::CardDef &def)
        {
            return def.self_rescue;
        }

        /**
         * @brief  该定义能否作为一次濒死救场牌。
         * @param[in] def     卡牌定义。
         * @param[in] is_self saver 是否为濒死者本人。
         * @return rescue 牌（桃）对任意 saver 成立；self_rescue 牌（酒）仅对
         *         濒死者本人成立。
         */
        inline bool can_rescue_def(const card::CardDef &def, bool is_self)
        {
            return is_rescue_def(def) || (is_self && is_self_rescue_def(def));
        }

        /**
         * @brief  该定义是否可作无懈响应牌。
         * @param[in] def 卡牌定义。
         * @return 数据标记 `counter` 为 true 时成立（不再认 id）。
         */
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
         * @brief  按卡牌定义分类出牌阶段的打出路径。
         * @param[in] def 卡牌定义。
         * @return 该定义对应的打出路径分类。
         * @note  纯静态分类（只看定义）：先装备、再延时锦囊、后有无主动效果；
         *        「杀」是状态规则（依赖回合上下文），保持正交谓词
         *        `is_sha_kind`，不作第 5 个分类值。
         */
        PlayClass classify_action(const card::CardDef &def);
    }
}

#endif  // INCLUDE_TKW_GAME_EFFECT_HPP
