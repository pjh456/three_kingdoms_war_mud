/**
 * @file skill.hpp
 * @brief 武将触发技结算：受到伤害后按固定顺序询问并落子。
 * @details 与装备可选触发同范式——结算点直接调用决策接缝（不订阅事件）；无
 *          武将或无对应技能时首行短路，保证无名座位逐字节零行为。
 * @note 触发钩子内的结算须有重入保护：本期反馈只移牌、不再次造成伤害；后续
 *       出现会造伤的触发技须显式加守卫。
 * @ingroup tkw_game_resolve
 */

#ifndef INCLUDE_TKW_GAME_SKILL_HPP
#define INCLUDE_TKW_GAME_SKILL_HPP

#include <string>

#include "card/card.hpp"
#include "card/def.hpp"
#include "game/core/card_event.hpp"
#include "game/core/context.hpp"
#include "game/core/decision.hpp"
#include "game/core/state.hpp"
#include "game/query/hero.hpp"
#include "hero/def.hpp"

namespace tkw
{
    namespace game
    {
        /**
         * @brief 反馈：受伤者获得伤害来源一张牌（手牌/装备区/判定区）。
         * @param[in] ctx    对局上下文。
         * @param[in] ai     决策源；询问是否发动技能与取哪张牌。
         * @param[in] victim 受到伤害的实体 id（技能拥有者）。
         * @param[in] source 伤害来源实体 id（空串 = 无来源，如闪电）。
         * @post 成功时该牌已移入 `victim` 手牌并发 `CardMoved` 事件；装备区失去
         *       时联动白银狮子回血。任一步失败安全返回，不部分落子。
         * @note 触发时点固定在伤害实际发生之后、濒死判定之前；来源无牌可取时
         *       不打开决策窗。取隐藏手牌经 resolve_target_pick 暗抽（消费 rng，
         *       与顺手牵羊同点）；装备区失去时联动白银狮子回血。
         * @note 任一步失败安全返回，不部分落子。
         */
        inline void trigger_fankui(
            GameContext &ctx, DecisionSource &ai, const std::string &victim,
            const std::string &source)
        {
            // 无来源或自伤不触发（技能要求伤害来源）
            if (source.empty() || source == victim)
                return;

            // 来源须为已离场者之外的实体
            const auto src = ctx.entities->find(source);
            if (src.is_none() || src.unwrap()->get_hp() <= 0)
                return;

            // 来源任一区域可取的牌：无牌则不打开决策窗
            if (ctx.cards->hand_size(source) == 0 &&
                ctx.cards->equip_size(source) == 0 &&
                ctx.cards->judge_size(source) == 0)
                return;

            if (!ai.trigger_hero_skill(
                    ctx, victim, hero::HeroSkill::FanKui, source))
                return;

            const auto pick = ai.pick_card_from_target(
                ctx, victim, source, PickCardScope::HandEquipJudge);
            if (pick.is_none())
                return;

            // 隐藏手牌按槽位经 rng 暗抽定位实体牌；明置牌直接携带身份
            const auto picked = resolve_target_pick(ctx, source, pick.unwrap());
            if (picked.is_none())
                return;
            const card::Card picked_card = picked.unwrap();

            card::Card removed;
            Zone from = Zone::Limbo;
            if (!remove_card_from_zones(
                    ctx, source, picked_card.instance_id, removed, &from))
                return;

            ctx.cards->add_to_hand(victim, removed);
            emit_card_moved(ctx, source, victim, removed, from, Zone::Hand);
            if (from == Zone::Equip)
                apply_equip_lost(ctx, source, removed);
        }

        /**
         * @brief 伤害落定后的武将触发钩子：对受伤者按固定顺序询问触发技。
         * @param[in] ctx     对局上下文。
         * @param[in] ai      决策源。
         * @param[in] victim  受到伤害的实体 id。
         * @param[in] source  伤害来源实体 id（空串 = 无来源）。
         * @param[in] applied 本次伤害实际扣减量；<= 0 表示未实际受伤，不触发。
         * @post 仅当 `applied > 0` 且受害者拥有对应技能时才可能改变状态。
         * @note 本期只分派反馈；后续触发技在此函数内按文档化固定顺序追加，保持
         *       单一入口与确定序。
         * @note 无对应技能时在打开任何决策窗/rng 消费之前短路。
         */
        inline void run_after_damage_skills(
            GameContext &ctx, DecisionSource &ai, const std::string &victim,
            const std::string &source, int applied)
        {
            if (applied <= 0)
                return;

            if (!has_hero_skill(ctx, victim, hero::HeroSkill::FanKui))
                return;
            trigger_fankui(ctx, ai, victim, source);
        }
    }
}

#endif  // INCLUDE_TKW_GAME_SKILL_HPP
