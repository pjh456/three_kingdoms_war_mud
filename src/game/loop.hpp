/**
 * @file loop.hpp
 * @brief 对局主循环：开局准备（建牌堆/洗牌/发初始手牌）→ 回合轮转 →
 *        结束判定（只剩一名存活玩家）。
 * @note 玩家实体由调用方先行创建（含座位与体力）；本模块只负责发牌与轮转。
 *       死亡者被 EntityManager 移除后自动跳过（next_player 按存活实体环绕）。
 */

#ifndef INCLUDE_TKW_GAME_LOOP_HPP
#define INCLUDE_TKW_GAME_LOOP_HPP

#include <cstdint>
#include <string>
#include <utility>

#include "card/catalog.hpp"
#include "card/manager.hpp"
#include "entity/manager.hpp"
#include "game/context.hpp"
#include "game/decision.hpp"
#include "game/state.hpp"
#include "game/turn.hpp"
#include "util/types.hpp"

namespace tkw
{
    namespace game
    {
        /** @brief 对局流程错误。 */
        enum class LoopError : std::uint8_t
        {
            NoPlayers,  /**< 场上没有玩家 */
            TurnFailed, /**< 某回合流程报错（如非法出牌脚本） */
            MaxRounds,  /**< 超出最大回合数（无法分出胜负的僵局） */
        };

        template <typename T>
        using LoopResult = Result<T, LoopError>;

        /** @brief 对局结果。 */
        struct GameOutcome
        {
            std::string winner; /**< 最后存活玩家 id；空串 = 无存活者（同归于尽） */
            int turns = 0;      /**< 实际进行的回合数（每执行一个玩家回合 +1） */
        };

        /**
         * @brief 可恢复的对局会话进度。
         * @note 只保存「下一回合角色 + 已执行回合数」；牌堆/实体/随机源状态在
         *       各容器里，存档时一并导出。可在任意回合边界暂停/继续。
         */
        struct GameSession
        {
            std::string current;  /**< 下一回合角色 id */
            int turns = 0;        /**< 已执行的回合数 */
            bool started = false; /**< 是否已开局准备（发牌） */
        };

        /** @brief 存活玩家数。 */
        inline std::size_t alive_count(const GameContext &ctx)
        {
            return ctx.entities->size();
        }

        /** @brief 会话是否已结束（存活 ≤ 1）。 */
        inline bool session_over(const GameContext &ctx)
        {
            return ctx.entities->size() <= 1;
        }

        /** @brief 会话胜者；空串 = 未结束或同归于尽。 */
        inline std::string session_winner(const GameContext &ctx)
        {
            if (ctx.entities->size() == 1)
                return (*ctx.entities->begin())->get_id();
            return {};
        }

        /** @brief 每名存活玩家发 count 张初始手牌（从堆顶摸，发布摸牌事件）。 */
        inline void deal_initial_hands(GameContext &ctx, int count)
        {
            for (const auto &ent : *ctx.entities)
                apply_draw(ctx, ent->get_id(), count);
        }

        /** @brief 开局准备：构建牌堆 → 洗牌 → 发初始手牌。 */
        inline void prepare_game(GameContext &ctx, int hand)
        {
            ctx.cards->build_deck(*ctx.catalog);
            if (ctx.rng)
                ctx.cards->shuffle_draw(*ctx.rng);
            deal_initial_hands(ctx, hand);
        }

        /**
         * @brief 开新局：准备牌堆并初始化会话（从 first_player 起）。
         * @param hand 每名玩家初始手牌数；< 0 时取规则配置的 initial_hand。
         */
        inline LoopResult<void> start_session(
            GameContext &ctx, GameSession &session, const std::string &first_player,
            int hand = -1)
        {
            if (ctx.entities->empty())
                return LoopResult<void>::Err(LoopError::NoPlayers);
            const int initial = hand >= 0 ? hand : rules_of(ctx).initial_hand;
            prepare_game(ctx, initial);
            session.current = first_player;
            session.turns = 0;
            session.started = true;
            return LoopResult<void>::Ok();
        }

        /**
         * @brief 执行会话的下一个回合并推进进度。
         * @note 存档恢复后直接调用即可续跑；回合失败/超上限返回 Err。
         */
        inline LoopResult<void> step_session(
            GameContext &ctx, DecisionSource &ai, GameSession &session)
        {
            if (session.current.empty())
                return LoopResult<void>::Err(LoopError::NoPlayers);
            auto r = execute_turn(ctx, ai, session.current);
            if (r.is_err())
                return LoopResult<void>::Err(LoopError::TurnFailed);
            session.current = next_player(ctx, session.current);
            if (++session.turns > rules_of(ctx).max_turns)
                return LoopResult<void>::Err(LoopError::MaxRounds);
            return LoopResult<void>::Ok();
        }

        /**
         * @brief 主循环：从 first_player 起轮转执行回合，直到只剩一名存活玩家。
         * @param hand 每名玩家初始手牌数；< 0 时取规则配置的 initial_hand。
         * @return Ok(GameOutcome) 或 Err(LoopError)。
         */
        inline LoopResult<GameOutcome> play_game(
            GameContext &ctx, DecisionSource &ai, const std::string &first_player,
            int hand = -1)
        {
            GameSession session;
            auto started = start_session(ctx, session, first_player, hand);
            if (started.is_err())
                return LoopResult<GameOutcome>::Err(started.unwrap_err());

            while (!session_over(ctx))
            {
                auto r = step_session(ctx, ai, session);
                if (r.is_err())
                    return LoopResult<GameOutcome>::Err(r.unwrap_err());
            }

            GameOutcome gr;
            gr.turns = session.turns;
            gr.winner = session_winner(ctx);
            return LoopResult<GameOutcome>::Ok(std::move(gr));
        }
    }
}

#endif  // INCLUDE_TKW_GAME_LOOP_HPP