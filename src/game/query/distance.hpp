/**
 * @file distance.hpp
 * @brief 距离与攻击范围：座次环距离 + 装备修正。
 * @details 规则约定：
 *          - 座次距离 = 存活者座位环上两下标的最短弧长（死亡者退出环，座位
 *            不回填）；
 *          - 攻击范围 = 武器 range，无武器为 1；
 *          - -1马（进攻马）：使用者计算到他人距离 -1；
 *          - +1马（防御马）：他人计算到自己的距离 +1；
 *          - 距离下限为 1。
 * @ingroup tkw_game_query
 */

#ifndef INCLUDE_TKW_GAME_DISTANCE_HPP
#define INCLUDE_TKW_GAME_DISTANCE_HPP

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include "card/def.hpp"
#include "game/core/context.hpp"
#include "game/query/hero.hpp"
#include "util/types.hpp"

namespace tkw
{
    namespace game
    {
        /** @brief 从装备区解析出的距离相关摘要。 */
        struct EquipSummary
        {
            int weapon_range = 0;         /**< 0 = 无武器（基础攻击距离 1） */
            bool offensive_horse = false; /**< -1马 */
            bool defensive_horse = false; /**< +1马 */
        };

        /**
         * @brief 距离与攻击范围只读查询（静态工具类）。
         * @details 纯函数集合：不持有上下文，`ctx` 由调用方传入；本类不改变任何状态。
         */
        class DistanceQuery
        {
        public:
            /** @brief 静态工具类，不可实例化。 */
            DistanceQuery() = delete;

            /**
             * @brief  座次距离（存活者环）：min(|i-j|, n-|i-j|)。
             * @param[in] ctx 只读上下文。
             * @param[in] a   实体 id。
             * @param[in] b   实体 id。
             * @return 两实体都在容器内时返回环上最短弧长；任一不存在时返回 0。
             * @note   i/j 为 `ordered_ids()` 中按座位升序的下标。死亡者已从容器
             *         移除但座位号不回填，故不能拿「绝对座位差」与「存活数」直接
             *         相减，须先取存活者座位环上的下标。
             */
            static int seat_distance(
                const ReadOnlyContext &ctx, const std::string &a,
                const std::string &b);

            /**
             * @brief  解析某实体装备区：武器 range 与坐骑方向（经 catalog）。
             * @param[in] ctx       只读上下文。
             * @param[in] entity_id 实体 id。
             * @return 距离相关摘要；无武器时 `weapon_range == 0`。
             * @post  本接口不改变任何状态。
             */
            static EquipSummary summarize_equipment(
                const ReadOnlyContext &ctx, const std::string &entity_id);

            /**
             * @brief  from 到 to 的调整后距离。
             * @param[in] ctx  只读上下文。
             * @param[in] from 起点实体 id。
             * @param[in] to   终点实体 id。
             * @return 含进攻/防御马与「马术」修正后的距离，下限 1。
             * @post  本接口不改变任何状态。
             */
            static int distance_between(
                const ReadOnlyContext &ctx, const std::string &from,
                const std::string &to);

            /**
             * @brief  距离判定：from 到 to 的距离（含马修正）是否 ≤ range。
             * @param[in] ctx   只读上下文。
             * @param[in] from  起点实体 id。
             * @param[in] to    终点实体 id。
             * @param[in] range 距离上限。
             * @return 调整后距离不超过 `range` 时为 true。
             * @note  顺手牵羊（`range == 1`）等按距离结算的牌走这里。
             * @post  本接口不改变任何状态。
             */
            static bool distance_le(
                const ReadOnlyContext &ctx, const std::string &from,
                const std::string &to, int range);

            /**
             * @brief  攻击距离判定：from 能否攻击 to。
             * @param[in] ctx  只读上下文。
             * @param[in] from 攻击方实体 id。
             * @param[in] to   目标实体 id。
             * @return 距离在武器攻击范围内时为 true（无武器时范围 1）。
             * @post  本接口不改变任何状态。
             */
            static bool in_attack_range(
                const ReadOnlyContext &ctx, const std::string &from,
                const std::string &to);
        };

        inline int DistanceQuery::seat_distance(
            const ReadOnlyContext &ctx, const std::string &a, const std::string &b)
        {
            const auto ids = ctx.entities->ordered_ids();
            const auto ia = std::find(ids.begin(), ids.end(), a);
            const auto ib = std::find(ids.begin(), ids.end(), b);
            if (ia == ids.end() || ib == ids.end())
                return 0;
            const int n = static_cast<int>(ids.size());
            const int i = static_cast<int>(ia - ids.begin());
            const int j = static_cast<int>(ib - ids.begin());
            const int d = std::abs(i - j);
            return std::min(d, n - d);
        }

        inline EquipSummary DistanceQuery::summarize_equipment(
            const ReadOnlyContext &ctx, const std::string &entity_id)
        {
            EquipSummary s;
            for (const auto &c : ctx.cards->equip(entity_id))
            {
                const auto def = ctx.catalog->find(c.def_id);
                if (def.is_none())
                    continue;
                const auto &eq = def.unwrap()->equip;
                if (eq.is_none())
                    continue;
                const auto &e = eq.unwrap();
                if (e.slot == card::EquipSlot::Weapon)
                    s.weapon_range = e.range;
                else if (e.slot == card::EquipSlot::OffensiveHorse)
                    s.offensive_horse = true;
                else if (e.slot == card::EquipSlot::DefensiveHorse)
                    s.defensive_horse = true;
            }
            return s;
        }

        inline int DistanceQuery::distance_between(
            const ReadOnlyContext &ctx, const std::string &from,
            const std::string &to)
        {
            int d = seat_distance(ctx, from, to);
            const auto fs = summarize_equipment(ctx, from);
            const auto ts = summarize_equipment(ctx, to);
            if (fs.offensive_horse)
                --d;
            if (ts.defensive_horse)
                ++d;

            // 锁定技「马术」：from 计算到其他角色的距离再 -1（下限 1 不变）
            if (HeroQuery::has_hero_skill(ctx, from, hero::HeroSkill::MaShu))
                --d;
            return std::max(d, 1);
        }

        inline bool DistanceQuery::distance_le(
            const ReadOnlyContext &ctx, const std::string &from,
            const std::string &to, int range)
        {
            return distance_between(ctx, from, to) <= range;
        }

        inline bool DistanceQuery::in_attack_range(
            const ReadOnlyContext &ctx, const std::string &from,
            const std::string &to)
        {
            const int range = summarize_equipment(ctx, from).weapon_range;
            return distance_le(ctx, from, to, range > 0 ? range : 1);
        }
    }
}

#endif  // INCLUDE_TKW_GAME_DISTANCE_HPP
