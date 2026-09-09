/**
 * @file judge.hpp
 * @brief 延时锦囊目标规则：判定区同名叠加检查、scope 合法性、去重后的合法目标集。
 * @note 只读查询：回合流程（放置预检）与动作枚举（AI 侧）共用同一份规则。
 */

#ifndef INCLUDE_TKW_GAME_JUDGE_HPP
#define INCLUDE_TKW_GAME_JUDGE_HPP

#include <string>
#include <vector>

#include "card/def.hpp"
#include "game/core/context.hpp"

namespace tkw
{
    namespace game
    {
        /** @brief 该实体判定区是否已有同名延时锦囊（判定区不可叠加）。 */
        inline bool has_same_delayed(
            const GameContext &ctx, const std::string &entity,
            const std::string &def_id)
        {
            for (const auto &c : ctx.cards->judge(entity))
                if (c.def_id == def_id)
                    return true;
            return false;
        }

        /**
         * @brief 目标是否在该延时锦囊 judge.scope 的合法集合内（纯谓词）。
         * @note scope=Self → 目标须为 player 本人；否则 → 目标须为他人。
         */
        inline bool is_delayed_scope_target(
            const std::string &player, const card::CardDef &def,
            const std::string &target)
        {
            const auto scope =
                def.judge.unwrap().scope.unwrap_or(card::Scope::Self);
            return scope == card::Scope::Self ? target == player : target != player;
        }

        /**
         * @brief 延时锦囊去重后的合法目标集（座位序，供枚举用）。
         * @note 先取 scope 合法集，再剔除判定区已有同名延时锦囊的目标。
         */
        inline std::vector<std::string> delayed_legal_targets(
            const GameContext &ctx, const std::string &player,
            const card::CardDef &def)
        {
            std::vector<std::string> out;
            if (is_delayed_scope_target(player, def, player) &&
                !has_same_delayed(ctx, player, def.id))
                out.push_back(player);
            for (const auto &e : *ctx.entities)
            {
                const std::string &t = e->get_id();
                if (t == player)
                    continue;
                if (is_delayed_scope_target(player, def, t) &&
                    !has_same_delayed(ctx, t, def.id))
                    out.push_back(t);
            }
            return out;
        }
    }
}

#endif  // INCLUDE_TKW_GAME_JUDGE_HPP
