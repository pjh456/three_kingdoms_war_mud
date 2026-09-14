/**
 * @file   judge.cpp
 * @brief  `JudgeQuery` 延时锦囊目标规则查询的定义。
 * @details 承载判定区同名检查、移送目标查找、scope 合法性与去重目标集。
 * @ingroup tkw_game_query
 */

#include "game/query/judge.hpp"

#include <string>
#include <vector>

namespace tkw
{
    namespace game
    {
        bool JudgeQuery::has_same_delayed(
            const ReadOnlyContext &ctx, const std::string &entity,
            const std::string &def_id)
        {
            for (const auto &c : ctx.cards->judge(entity))
                if (c.def_id == def_id)
                    return true;
            return false;
        }

        std::string JudgeQuery::next_delayed_target(
            const ReadOnlyContext &ctx, const std::string &player,
            const std::string &def_id)
        {
            const auto order = ctx.entities->order_from(ctx.entities->next(player));
            for (const auto &id : order)
                if (!has_same_delayed(ctx, id, def_id))
                    return id;
            return {};
        }

        bool JudgeQuery::is_delayed_scope_target(
            const ReadOnlyContext &ctx, const std::string &player,
            const card::CardDef &def, const std::string &target)
        {
            const auto &judge = def.judge.unwrap();
            const auto scope = judge.scope.unwrap_or(card::Scope::Self);
            if (scope == card::Scope::Self)
                return target == player;
            if (target == player)
                return false;
            return judge.range <= 0 ||
                   HeroQuery::ignores_trick_distance(ctx, player) ||
                   DistanceQuery::distance_le(ctx, player, target, judge.range);
        }

        std::vector<std::string> JudgeQuery::delayed_legal_targets(
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
