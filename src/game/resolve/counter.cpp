/**
 * @file   counter.cpp
 * @brief  无懈可击结算的函数体定义。
 * @details 实现 `counter.hpp` 声明的无懈牌查询、消费与窗口轮询；链状态仅在单次
 *          窗口内保留。
 * @ingroup tkw_game_resolve
 */

#include "game/resolve/counter.hpp"

#include <string>
#include <vector>

namespace tkw
{
    namespace game
    {
        bool has_counter_card(const GameContext &ctx, const std::string &player)
        {
            return StateQuery::any_hand_card_matching(
                ctx, player,
                [](const card::CardDef &def) { return is_counter_def(def); });
        }

        bool consume_counter(
            GameContext &ctx, const std::string &player,
            const std::string &instance_id)
        {
            return StateOps(ctx).consume_hand_card_matching(player, instance_id,
                       [](const card::CardDef &def, const card::Card &)
                       { return is_counter_def(def); },
                       DiscardKind::Response)
                .is_some();
        }

        bool CounterResolver::try_play_counter(
            const std::string &player, const CounterWindow &window,
            int counter_played)
        {
            if (!has_counter_card(m_ctx, player))
                return false;
            const auto chosen = m_ai.play_counter(
                m_ctx, player, window.trick_user, window.targets,
                window.trick ? window.trick->id : std::string(), counter_played);
            if (chosen.is_none())
                return false;
            return consume_counter(m_ctx, player, chosen.unwrap());
        }

        bool CounterResolver::resolve_nullification(
            const CounterWindow &window)
        {
            const std::string &start =
                window.trick_user.empty() ? window.targets.front()
                                          : window.trick_user;
            const auto order = seat_order_from(m_ctx, start);
            bool cancelled = false;
            int played = 0;
            for (int round = 0; round < rules_of(m_ctx).wuxie_rounds; ++round)
            {
                bool any = false;
                for (const auto &p : order)
                {
                    if (try_play_counter(p, window, played))
                    {
                        cancelled = !cancelled;
                        any = true;
                        ++played;
                    }
                }
                if (!any)
                    break;
            }
            return cancelled;
        }
    }
}
