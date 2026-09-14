/**
 * @file judge.hpp
 * @brief 延时锦囊目标规则：判定区同名叠加检查、scope 合法性、去重后的合法目标集。
 * @note 只读查询：回合流程（放置预检）与动作枚举（AI 侧）共用同一份规则。
 * @ingroup tkw_game_query
 */

#ifndef INCLUDE_TKW_GAME_JUDGE_HPP
#define INCLUDE_TKW_GAME_JUDGE_HPP

#include <string>
#include <vector>

#include "card/def.hpp"
#include "game/core/context.hpp"
#include "game/query/distance.hpp"
#include "game/query/hero.hpp"

namespace tkw
{
    namespace game
    {
        /**
         * @brief  该实体判定区是否已有同名延时锦囊（判定区不可叠加）。
         * @param[in] ctx    只读上下文。
         * @param[in] entity 实体 id。
         * @param[in] def_id 延时锦囊的 def id。
         * @return 判定区已有同名延时锦囊时为 true。
         * @post  本接口不改变任何状态。
         */
        inline bool has_same_delayed(
            const ReadOnlyContext &ctx, const std::string &entity,
            const std::string &def_id)
        {
            for (const auto &c : ctx.cards->judge(entity))
                if (c.def_id == def_id)
                    return true;
            return false;
        }

        /**
         * @brief  失败闪电类延时锦囊的移送目标。
         * @details 从下家起按座位序找判定区无同名延时锦囊的存活者（环绕，当前
         *          角色排最后）。
         * @param[in] ctx    只读上下文。
         * @param[in] player 当前角色 id（环绕时排在最后）。
         * @param[in] def_id 延时锦囊的 def id。
         * @return 合法的移送目标 id；找不到时为空串（调用方须弃置该牌）。
         * @note  判定区不可叠加同名延时锦囊；死亡者已从容器移除故天然跳过。
         */
        inline std::string next_delayed_target(
            const ReadOnlyContext &ctx, const std::string &player,
            const std::string &def_id)
        {
            const auto order = ctx.entities->order_from(ctx.entities->next(player));
            for (const auto &id : order)
                if (!has_same_delayed(ctx, id, def_id))
                    return id;
            return {};
        }

        /**
         * @brief  目标是否在该延时锦囊 `judge.scope` 的合法集合内（纯谓词）。
         * @param[in] ctx    只读上下文。
         * @param[in] player 使用延时锦囊的玩家 id。
         * @param[in] def    延时锦囊的卡牌定义。
         * @param[in] target 待校验的目标 id。
         * @return 目标合法时为 true。
         * @note   `scope == Self` → 目标须为 `player` 本人；否则 → 目标须为他
         *         人，`judge.range > 0` 时还须在距离上限内（兵粮寸断距离 1）。
         * @pre    `def.judge` 已绑定。
         */
        inline bool is_delayed_scope_target(
            const ReadOnlyContext &ctx, const std::string &player,
            const card::CardDef &def, const std::string &target)
        {
            const auto &judge = def.judge.unwrap();
            const auto scope = judge.scope.unwrap_or(card::Scope::Self);
            if (scope == card::Scope::Self)
                return target == player;
            if (target == player)
                return false;
            return judge.range <= 0 || ignores_trick_distance(ctx, player) ||
                   DistanceQuery::distance_le(ctx, player, target, judge.range);
        }

        /**
         * @brief  延时锦囊去重后的合法目标集（座位序，供枚举用）。
         * @param[in] ctx    只读上下文。
         * @param[in] player 使用延时锦囊的玩家 id。
         * @param[in] def    延时锦囊的卡牌定义。
         * @return 合法目标 id 列表（按座位序）。
         * @note  先取 scope 合法集，再剔除判定区已有同名延时锦囊的目标。
         * @pre    `def.judge` 已绑定。
         * @post  本接口不改变任何状态。
         */
        inline std::vector<std::string> delayed_legal_targets(
            const ReadOnlyContext &ctx, const std::string &player,
            const card::CardDef &def)
        {
            std::vector<std::string> out;
            if (is_delayed_scope_target(ctx, player, def, player) &&
                !has_same_delayed(ctx, player, def.id))
                out.push_back(player);
            for (const auto *e : ctx.entities->const_view())
            {
                const std::string &t = e->get_id();
                if (t == player)
                    continue;
                if (is_delayed_scope_target(ctx, player, def, t) &&
                    !has_same_delayed(ctx, t, def.id))
                    out.push_back(t);
            }
            return out;
        }
    }
}

#endif  // INCLUDE_TKW_GAME_JUDGE_HPP
