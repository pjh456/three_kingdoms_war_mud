/**
 * @file   loop.cpp
 * @brief  开局准备、回合轮转与终局判定的定义。
 * @ingroup tkw_game_flow
 */

#include "game/flow/loop.hpp"

#include <cstddef>
#include <string>
#include <utility>

namespace tkw
{
    namespace game
    {
        bool SessionQuery::identity_over(const GameContext &ctx)
        {
            if (ctx.entities->empty())
                return true;

            bool lord_alive = false;
            bool hostile_alive = false;
            for (const auto *e : ctx.entities->const_view())
            {
                switch (role_of(ctx, e->get_id()))
                {
                case Role::Lord:
                    lord_alive = true;
                    break;
                case Role::Rebel:
                case Role::Traitor:
                    hostile_alive = true;
                    break;
                default:
                    break;
                }
            }

            return !lord_alive || !hostile_alive;
        }

        bool SessionQuery::session_over(const GameContext &ctx)
        {
            if (mode_of(ctx) == GameMode::Brawl)
                return ctx.entities->size() <= 1;  // 乱斗口径逐字不变
            return SessionQuery::identity_over(ctx);
        }

        WinCamp SessionQuery::session_camp(const GameContext &ctx)
        {
            if (!SessionQuery::session_over(ctx) || mode_of(ctx) == GameMode::Brawl)
                return WinCamp::None;
            if (ctx.entities->empty())
                return WinCamp::Draw;

            bool lord_alive = false;
            bool traitor_alive = false;
            for (const auto *e : ctx.entities->const_view())
            {
                switch (role_of(ctx, e->get_id()))
                {
                case Role::Lord:
                    lord_alive = true;
                    break;
                case Role::Traitor:
                    traitor_alive = true;
                    break;
                default:
                    break;
                }
            }

            if (lord_alive)
                return WinCamp::LordCamp;
            if (ctx.entities->size() == 1 && traitor_alive)
                return WinCamp::TraitorCamp;
            return WinCamp::RebelCamp;
        }

        std::string SessionQuery::session_winner(const GameContext &ctx)
        {
            if (mode_of(ctx) == GameMode::Brawl)
            {
                if (ctx.entities->size() == 1)
                    return (*ctx.entities->begin())->get_id();
                return {};
            }

            switch (SessionQuery::session_camp(ctx))
            {
            case WinCamp::LordCamp:
                for (const auto *e : ctx.entities->const_view())
                    if (role_of(ctx, e->get_id()) == Role::Lord)
                        return e->get_id();
                return {};
            case WinCamp::TraitorCamp:
                for (const auto *e : ctx.entities->const_view())
                    if (role_of(ctx, e->get_id()) == Role::Traitor)
                        return e->get_id();
                return {};
            case WinCamp::RebelCamp:
                if (ctx.roles)
                    for (const auto &entry : *ctx.roles)
                        if (entry.second == Role::Rebel)
                            return entry.first;
                return {};
            default:
                return {};
            }
        }

        std::string SessionQuery::next_after_seat(
            const GameContext &ctx, int seat)
        {
            const auto ids = ctx.entities->ordered_ids();  // 按座位升序
            if (ids.empty())
                return {};
            for (const auto &id : ids)
            {
                const auto e = ctx.entities->find(id);
                if (e.is_some() && e.unwrap()->get_seat() > seat)
                    return id;
            }
            return ids.front();
        }

        void GameSetup::deal_initial_hands(int count)
        {
            for (const auto &ent : *m_ctx.entities)
                StateOps(m_ctx).apply_draw(ent->get_id(), count);
        }

        void GameSetup::prepare_game(int hand)
        {
            m_ctx.cards->build_deck(*m_ctx.catalog);
            if (m_ctx.rng)
                m_ctx.cards->shuffle_draw(*m_ctx.rng);
            deal_initial_hands(hand);
        }

        LoopResult<void> GameSetup::start_session(
            GameSession &session, const std::string &first_player, int hand)
        {
            if (m_ctx.entities->empty())
                return LoopResult<void>::Err(LoopError::NoPlayers);
            const int initial = hand >= 0 ? hand : rules_of(m_ctx).initial_hand;
            prepare_game(initial);
            session.current = first_player;
            session.turns = 0;
            session.started = true;
            return LoopResult<void>::Ok();
        }

        LoopResult<void> GameLoop::step_session(
            GameSession &session, TurnError *root)
        {
            const auto actor = m_ctx.entities->find(session.current);
            if (actor.is_none())
                return LoopResult<void>::Err(LoopError::NoPlayers);
            const int seat = actor.unwrap()->get_seat();

            auto r = TurnFlow(m_ctx, m_ai).execute_turn(session.current);
            const bool failed = r.is_err();
            if (failed && root)
                *root = r.unwrap_err();

            // 失败回合已部分结算且不可回滚：无论成败都消费该回合并推进到下一角色，
            // 避免再次 step 重跑同一角色、重放判定/摸牌/出牌效果。
            if (m_ctx.entities->find(session.current).is_some())
                session.current = TurnQuery::next_player(m_ctx, session.current);
            else
                session.current = SessionQuery::next_after_seat(m_ctx, seat);  // 回合中死亡

            ++session.turns;

            if (failed)
                return LoopResult<void>::Err(LoopError::TurnFailed);

            // 终局判定先于回合上限：本回合已终局（含乱斗 0 存活同归于尽）则上限
            // 不再适用；仅未终局且越上限才算无法分出胜负的僵局。
            const bool over = SessionQuery::session_over(m_ctx);
            if (!over && session.turns > rules_of(m_ctx).max_turns)
                return LoopResult<void>::Err(LoopError::MaxRounds);
            return LoopResult<void>::Ok();
        }

        LoopResult<GameOutcome> GameLoop::play_game(
            const std::string &first_player, int hand)
        {
            GameSession session;
            auto started =
                GameSetup(m_ctx).start_session(session, first_player, hand);
            if (started.is_err())
                return LoopResult<GameOutcome>::Err(started.unwrap_err());

            while (!SessionQuery::session_over(m_ctx))
            {
                auto r = step_session(session);
                if (r.is_err())
                    return LoopResult<GameOutcome>::Err(r.unwrap_err());
            }

            GameOutcome gr;
            gr.turns = session.turns;
            gr.winner = SessionQuery::session_winner(m_ctx);
            gr.camp = SessionQuery::session_camp(m_ctx);
            return LoopResult<GameOutcome>::Ok(std::move(gr));
        }
    }
}
