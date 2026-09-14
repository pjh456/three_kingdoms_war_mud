/**
 * @file   skill.cpp
 * @brief  武将触发技结算的函数体定义。
 * @details 实现 `skill.hpp` 声明的反馈落子与伤害后触发分派；无对应技能时在任何
 *          决策窗/rng 消费之前短路。
 * @ingroup tkw_game_resolve
 */

#include "game/resolve/skill.hpp"

#include <string>

namespace tkw
{
    namespace game
    {
        void trigger_fankui(
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
            const auto picked = StateOps(ctx).resolve_target_pick(source, pick.unwrap());
            if (picked.is_none())
                return;
            const card::Card picked_card = picked.unwrap();

            card::Card removed;
            Zone from = Zone::Limbo;
            if (!StateOps(ctx).remove_card_from_zones(source, picked_card.instance_id, removed, &from))
                return;

            ctx.cards->add_to_hand(victim, removed);
            emit_card_moved(ctx, source, victim, removed, from, Zone::Hand);
            if (from == Zone::Equip)
                StateOps(ctx).apply_equip_lost(source, removed);
        }

        void run_after_damage_skills(
            GameContext &ctx, DecisionSource &ai, const std::string &victim,
            const std::string &source, int applied)
        {
            if (applied <= 0)
                return;

            if (!HeroQuery::has_hero_skill(ctx, victim, hero::HeroSkill::FanKui))
                return;
            trigger_fankui(ctx, ai, victim, source);
        }
    }
}
