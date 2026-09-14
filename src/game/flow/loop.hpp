/**
 * @file loop.hpp
 * @brief 对局主循环：开局准备、回合轮转与结束判定。
 * @details 开局准备（建牌堆/洗牌/发初始手牌）→ 回合轮转 → 结束判定（乱斗 =
 *          只剩一名存活玩家；身份局 = 主公阵亡或敌对尽灭）。玩家实体由调用方
 *          先行创建（含座位与体力）；本模块只负责发牌与轮转，死亡者被
 *          `EntityManager` 移除后自动跳过（`next_player` 按存活实体环绕）。
 * @ingroup tkw_game_flow
 */

#ifndef INCLUDE_TKW_GAME_LOOP_HPP
#define INCLUDE_TKW_GAME_LOOP_HPP

#include <cstdint>
#include <string>
#include <utility>

#include "card/catalog.hpp"
#include "card/manager.hpp"
#include "entity/manager.hpp"
#include "game/core/context.hpp"
#include "game/core/decision.hpp"
#include "game/core/roles.hpp"
#include "game/core/state.hpp"
#include "game/flow/turn.hpp"
#include "util/types.hpp"

namespace tkw
{
    namespace game
    {
        /** @brief 对局流程错误。 */
        enum class LoopError : std::uint8_t
        {
            NoPlayers,  /**< 场上没有玩家。 */
            TurnFailed, /**< 某回合流程报错（如非法出牌脚本）。 */
            MaxRounds,  /**< 超出最大回合数（无法分出胜负的僵局）。 */
        };

        /**
         * @brief 对局流程结果别名。
         * @tparam T 成功时承载的值类型。
         */
        template <typename T>
        using LoopResult = Result<T, LoopError>;

        /** @brief 对局结果。 */
        struct GameOutcome
        {
            std::string winner; /**< 胜者/阵营代表 id；空串 = 无存活者或未标定。 */
            int turns = 0;      /**< 实际进行的回合数（每执行一个玩家回合 +1）。 */
            WinCamp camp = WinCamp::None; /**< 胜利阵营；乱斗恒 `None`。 */
        };

        /**
         * @brief 可恢复的对局会话进度。
         * @note 只保存「下一回合角色 + 已执行回合数」；牌堆/实体/随机源状态在
         *       各容器里，存档时一并导出。可在任意回合边界暂停/继续。
         */
        struct GameSession
        {
            std::string current;  /**< 下一回合角色 id。 */
            int turns = 0;        /**< 已执行的回合数。 */
            bool started = false; /**< 是否已开局准备（发牌）。 */
        };

        /**
         * @brief  存活玩家数。
         * @param[in] ctx 只读上下文。
         * @return 实体容器中的实体数量。
         */
        inline std::size_t alive_count(const GameContext &ctx)
        {
            return ctx.entities->size();
        }

        /**
         * @brief  身份局是否终局。
         * @details 终局条件：主公阵亡，或反贼与内奸尽灭；空场亦终局。
         * @param[in] ctx 只读上下文。
         * @return 空场或无可判定的存活主公/敌对时返回 true。
         * @note  只在 `mode == Identity` 时由 `session_over` 调用；角色表缺失或
         *         角色为 `None` 时视为无主无敌对（立即终局），属非法建局态，由
         *         建局分配与存档交叉校验兜底。
         */
        inline bool identity_over(const GameContext &ctx)
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

        /**
         * @brief  会话是否已结束。
         * @details 乱斗 = 存活 ≤ 1；身份局 = 主公阵亡或敌对尽灭。
         * @param[in] ctx 只读上下文。
         * @return 已终局时为 true；乱斗口径逐字不变。
         */
        inline bool session_over(const GameContext &ctx)
        {
            if (mode_of(ctx) == GameMode::Brawl)
                return ctx.entities->size() <= 1;  // 乱斗口径逐字不变
            return identity_over(ctx);
        }

        /**
         * @brief  会话胜利阵营。
         * @param[in] ctx 只读上下文。
         * @return 未结束或乱斗返回 `None`；身份局按阵营终局口径：0 存活 `Draw`；
         *         主公存活 `LordCamp`；主公阵亡且内奸为唯一存活者 `TraitorCamp`；
         *         其余主公阵亡情形 `RebelCamp`。
         * @note  0 存活优先于任何「主公阵亡」判定。
         */
        inline WinCamp session_camp(const GameContext &ctx)
        {
            if (!session_over(ctx) || mode_of(ctx) == GameMode::Brawl)
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

        /**
         * @brief  会话胜者/阵营代表 id；乱斗口径逐字不变。
         * @param[in] ctx 只读上下文。
         * @return 乱斗 = 唯一存活者 id，否则空串；身份局 `LordCamp` = 存活主公、
         *         `TraitorCamp` = 存活内奸、`RebelCamp` = 角色表首个反贼（按 id
         *         序，允许已阵亡，稳定展示）、`Draw`/`None` = 空串。
         */
        inline std::string session_winner(const GameContext &ctx)
        {
            if (mode_of(ctx) == GameMode::Brawl)
            {
                if (ctx.entities->size() == 1)
                    return (*ctx.entities->begin())->get_id();
                return {};
            }

            switch (session_camp(ctx))
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

        /**
         * @brief  座位严格大于 `seat` 的第一个存活者（环绕到最小座位）。
         * @param[in] ctx  只读上下文。
         * @param[in] seat 参照座位号。
         * @return 下一个存活者 id；容器为空时返回空串。
         */
        inline std::string next_after_seat(const GameContext &ctx, int seat)
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

        /**
         * @brief  每名存活玩家发 `count` 张初始手牌。
         * @param[in,out] ctx   对局上下文。
         * @param[in]     count 每人发牌数。
         * @post  从堆顶摸牌并发布摸牌事件。
         */
        inline void deal_initial_hands(GameContext &ctx, int count)
        {
            for (const auto &ent : *ctx.entities)
                apply_draw(ctx, ent->get_id(), count);
        }

        /**
         * @brief  开局准备：构建牌堆 → 洗牌 → 发初始手牌。
         * @param[in,out] ctx  对局上下文。
         * @param[in]     hand 每名玩家初始手牌数。
         * @note  `ctx.rng` 为空时跳过洗牌（供确定性测试）。
         */
        inline void prepare_game(GameContext &ctx, int hand)
        {
            ctx.cards->build_deck(*ctx.catalog);
            if (ctx.rng)
                ctx.cards->shuffle_draw(*ctx.rng);
            deal_initial_hands(ctx, hand);
        }

        /**
         * @brief  开新局：准备牌堆并初始化会话。
         * @param[in,out] ctx          对局上下文。
         * @param[out]    session      会话进度；被重置为从 `first_player` 起。
         * @param[in]     first_player 首位回合角色 id。
         * @param[in]     hand         每名玩家初始手牌数；`< 0` 时取规则配置的
         *                             `initial_hand`。
         * @return 结算结果。
         * @retval Ok  牌堆已建好、洗牌与发牌完成，会话已初始化。
         * @retval Err(LoopError::NoPlayers) 场上没有玩家。
         * @post  成功时 `session.current = first_player`、`session.turns = 0`、
         *         `session.started = true`。
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
         * @brief  执行会话的下一个回合并推进进度。
         * @param[in,out] ctx     对局上下文。
         * @param[in,out] ai      决策源。
         * @param[in,out] session 会话进度；成功/失败均推进到下一角色并 +1 回合。
         * @param[out]    root    非空时仅在 `TurnFailed` 分支写入回合根因；成功、
         *                        `NoPlayers` 与 `MaxRounds` 路径不写，由调用方按
         *                        code 分流。
         * @return 结算结果。
         * @retval Ok  回合已消费，且会话未终局、未越回合上限。
         * @retval Err(LoopError::NoPlayers) `session.current` 指向的角色已不存在。
         * @retval Err(LoopError::TurnFailed) 回合流程失败；状态已就地落子且不可回滚。
         * @retval Err(LoopError::MaxRounds) 未终局且已越 `rules.max_turns`。
         * @note  存档恢复后直接调用即可续跑。回合失败不等于状态未变：`execute_turn`
         *         各阶段已就地落子且不可回滚，因此失败回合同样被消费并推进（下一个
         *         角色 + 回合数 +1），仅在推进后返回 `TurnFailed`，避免再次调用重跑
         *         同一角色、重放本回合效果。
         * @note  无论角色是否在回合中死亡都必定推进座位；终局判定先于回合上限。
         */
        inline LoopResult<void> step_session(
            GameContext &ctx, DecisionSource &ai, GameSession &session,
            TurnError *root = nullptr)
        {
            const auto actor = ctx.entities->find(session.current);
            if (actor.is_none())
                return LoopResult<void>::Err(LoopError::NoPlayers);
            const int seat = actor.unwrap()->get_seat();

            auto r = execute_turn(ctx, ai, session.current);
            const bool failed = r.is_err();
            if (failed && root)
                *root = r.unwrap_err();

            // 失败回合已部分结算且不可回滚：无论成败都消费该回合并推进到下一角色，
            // 避免再次 step 重跑同一角色、重放判定/摸牌/出牌效果。
            if (ctx.entities->find(session.current).is_some())
                session.current = next_player(ctx, session.current);
            else
                session.current = next_after_seat(ctx, seat);  // 回合中死亡

            ++session.turns;

            if (failed)
                return LoopResult<void>::Err(LoopError::TurnFailed);

            // 终局判定先于回合上限：本回合已终局（含乱斗 0 存活同归于尽）则上限
            // 不再适用；仅未终局且越上限才算无法分出胜负的僵局。
            const bool over = session_over(ctx);
            if (!over && session.turns > rules_of(ctx).max_turns)
                return LoopResult<void>::Err(LoopError::MaxRounds);
            return LoopResult<void>::Ok();
        }

        /**
         * @brief  主循环：从 `first_player` 起轮转执行回合，直到会话终局。
         * @param[in,out] ctx          对局上下文。
         * @param[in,out] ai           决策源。
         * @param[in]     first_player 首位回合角色 id。
         * @param[in]     hand         每名玩家初始手牌数；`< 0` 时取规则配置的
         *                             `initial_hand`。
         * @return 结算结果。
         * @retval Ok(GameOutcome) 会话终局，返回胜者/回合数/胜利阵营。
         * @retval Err(LoopError) 开局准备或某回合失败、超出回合上限；取值同
         *         `start_session`/`step_session`。
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
            gr.camp = session_camp(ctx);
            return LoopResult<GameOutcome>::Ok(std::move(gr));
        }
    }
}

#endif  // INCLUDE_TKW_GAME_LOOP_HPP