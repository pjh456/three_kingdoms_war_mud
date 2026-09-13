/**
 * @file hero.hpp
 * @brief 武将查询：按实体 id 解析其武将定义与技能。
 * @note 只读查询（query 层）：目录与实体都由上下文注入，本模块不持有状态。
 *       无目录 / 实体无武将 / 目录未命中一律回落 nullptr/false，使无名座位在
 *       全流程中与「无技能」等价，不新增失败路径。
 */

#ifndef INCLUDE_TKW_GAME_HERO_HPP
#define INCLUDE_TKW_GAME_HERO_HPP

#include <string>

#include "game/core/context.hpp"
#include "hero/catalog.hpp"
#include "hero/def.hpp"

namespace tkw
{
    namespace game
    {
        /**
         * @brief 返回实体所绑定武将的定义；无则 nullptr。
         * @return ctx.heroes 空、ctx.entities 空、实体不存在、实体 hero 为空或
         *         目录未命中 → nullptr；否则指向目录内定义。
         * @note 武将身份创建后不可变，返回指针在目录生命周期内稳定。
         */
        inline const hero::HeroDef *hero_of(
            const ReadOnlyContext &ctx, const std::string &entity_id)
        {
            if (ctx.heroes == nullptr || ctx.entities == nullptr)
                return nullptr;
            const auto found = ctx.entities->find(entity_id);
            if (found.is_none())
                return nullptr;
            const std::string &id = found.unwrap()->get_hero();
            if (id.empty())
                return nullptr;
            const auto def = ctx.heroes->find(id);
            return def.is_some() ? def.unwrap() : nullptr;
        }

        /**
         * @brief 实体是否拥有指定武将技能。
         * @return hero_of 未命中 → false；否则在其 skills 中查找。
         * @note 锁定技查询的唯一入口：行为消费点（如 sha_limit）经此判定，
         *       不直接读目录。
         */
        inline bool has_hero_skill(
            const ReadOnlyContext &ctx, const std::string &entity_id,
            hero::HeroSkill skill)
        {
            const hero::HeroDef *def = hero_of(ctx, entity_id);
            if (def == nullptr)
                return false;
            for (const auto s : def->skills)
                if (s == skill)
                    return true;
            return false;
        }

        /**
         * @brief 摸牌阶段摸牌张数：rules.draw_per_turn，锁定技「英姿」再 +1。
         * @return 无目录 / 实体无武将 / 无英姿 → rules.draw_per_turn；有英姿 +1。
         * @note 摸牌阶段张数的唯一采样点；兵粮寸断跳过整个摸牌阶段时不经本函数，
         *       故英姿不会越过跳过语义（跳过在 execute_turn 的调用闸门）。
         */
        inline int draw_phase_count(
            const ReadOnlyContext &ctx, const std::string &entity_id)
        {
            int count = rules_of(ctx).draw_per_turn;
            if (has_hero_skill(ctx, entity_id, hero::HeroSkill::YingZi))
                ++count;
            return count;
        }
    }
}

#endif  // INCLUDE_TKW_GAME_HERO_HPP
