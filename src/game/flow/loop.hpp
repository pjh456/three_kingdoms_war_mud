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
         * @brief 会话只读查询（静态工具类）。
         * @details 纯函数集合：不持有上下文，`ctx` 由调用方传入；本类不改变任何状态。
         */
        class SessionQuery
        {
        public:
            /** @brief 静态工具类，不可实例化。 */
            SessionQuery() = delete;

            /**
             * @brief  存活玩家数。
             * @param[in] ctx 只读上下文。
             * @return 实体容器中的实体数量。
             */
            static std::size_t alive_count(const GameContext &ctx);

            /**
             * @brief  身份局是否终局。
             * @details 终局条件：主公阵亡，或反贼与内奸尽灭；空场亦终局。
             * @param[in] ctx 只读上下文。
             * @return 空场或无可判定的存活主公/敌对时返回 true。
             * @note  只在 `mode == Identity` 时由 `session_over` 调用；角色表缺失或
             *         角色为 `None` 时视为无主无敌对（立即终局），属非法建局态，由
             *         建局分配与存档交叉校验兜底。
             */
            static bool identity_over(const GameContext &ctx);

            /**
             * @brief  会话是否已结束。
             * @details 乱斗 = 存活 ≤ 1；身份局 = 主公阵亡或敌对尽灭。
             * @param[in] ctx 只读上下文。
             * @return 已终局时为 true；乱斗口径逐字不变。
             */
            static bool session_over(const GameContext &ctx);

            /**
             * @brief  会话胜利阵营。
             * @param[in] ctx 只读上下文。
             * @return 未结束或乱斗返回 `None`；身份局按阵营终局口径：0 存活 `Draw`；
             *         主公存活 `LordCamp`；主公阵亡且内奸为唯一存活者 `TraitorCamp`；
             *         其余主公阵亡情形 `RebelCamp`。
             * @note  0 存活优先于任何「主公阵亡」判定。
             */
            static WinCamp session_camp(const GameContext &ctx);

            /**
             * @brief  会话胜者/阵营代表 id；乱斗口径逐字不变。
             * @param[in] ctx 只读上下文。
             * @return 乱斗 = 唯一存活者 id，否则空串；身份局 `LordCamp` = 存活主公、
             *         `TraitorCamp` = 存活内奸、`RebelCamp` = 角色表首个反贼（按 id
             *         序，允许已阵亡，稳定展示）、`Draw`/`None` = 空串。
             */
            static std::string session_winner(const GameContext &ctx);

            /**
             * @brief  座位严格大于 `seat` 的第一个存活者（环绕到最小座位）。
             * @param[in] ctx  只读上下文。
             * @param[in] seat 参照座位号。
             * @return 下一个存活者 id；容器为空时返回空串。
             */
            static std::string next_after_seat(const GameContext &ctx, int seat);
        };

        /**
         * @brief 开局准备（操作类）。
         * @details 持对局上下文（引用、非拥有），方法不再逐个传 `ctx`；负责建
         *          牌堆、洗牌、发初始手牌与会话初始化。
         * @warning 不拥有 `m_ctx`：被其引用的容器/目录须比本对象存活更久；不得跨局复用。
         */
        class GameSetup
        {
        public:
            /**
             * @brief  绑定对局上下文。
             * @param[in,out] ctx 对局上下文；本对象只持引用。
             */
            explicit GameSetup(GameContext &ctx) : m_ctx(ctx) {}

            /**
             * @brief  每名存活玩家发 `count` 张初始手牌。
             * @param[in] count 每人发牌数。
             * @post  从堆顶摸牌并发布摸牌事件。
             */
            void deal_initial_hands(int count);

            /**
             * @brief  开局准备：构建牌堆 → 洗牌 → 发初始手牌。
             * @param[in] hand 每名玩家初始手牌数。
             * @note  `m_ctx.rng` 为空时跳过洗牌（供确定性测试）。
             */
            void prepare_game(int hand);

            /**
             * @brief  开新局：准备牌堆并初始化会话。
             * @param[out] session      会话进度；被重置为从 `first_player` 起。
             * @param[in]  first_player 首位回合角色 id。
             * @param[in]  hand         每名玩家初始手牌数；`< 0` 时取规则配置的
             *                          `initial_hand`。
             * @return 结算结果。
             * @retval Ok  牌堆已建好、洗牌与发牌完成，会话已初始化。
             * @retval Err(LoopError::NoPlayers) 场上没有玩家。
             * @post  成功时 `session.current = first_player`、`session.turns = 0`、
             *         `session.started = true`。
             */
            LoopResult<void> start_session(
                GameSession &session, const std::string &first_player,
                int hand = -1);

        private:
            GameContext &m_ctx; /**< 对局上下文（引用，非拥有）。 */
        };

        /**
         * @brief 对局主循环（操作类）。
         * @details 持对局上下文与决策源（引用、非拥有），方法不再逐个传
         *          `ctx`/`ai`；负责按会话进度执行回合并判定终局。
         * @warning 不拥有 `m_ctx`/`m_ai`：二者须比本对象存活更久；不得跨局复用。
         */
        class GameLoop
        {
        public:
            /**
             * @brief  绑定对局上下文与决策源。
             * @param[in,out] ctx 对局上下文；本对象只持引用。
             * @param[in,out] ai  决策源；回合流程经它询问。
             */
            GameLoop(GameContext &ctx, DecisionSource &ai) : m_ctx(ctx), m_ai(ai) {}

            /**
             * @brief  执行会话的下一个回合并推进进度。
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
            LoopResult<void> step_session(
                GameSession &session, TurnError *root = nullptr);

            /**
             * @brief  主循环：从 `first_player` 起轮转执行回合，直到会话终局。
             * @param[in] first_player 首位回合角色 id。
             * @param[in] hand         每名玩家初始手牌数；`< 0` 时取规则配置的
             *                         `initial_hand`。
             * @return 结算结果。
             * @retval Ok(GameOutcome) 会话终局，返回胜者/回合数/胜利阵营。
             * @retval Err(LoopError) 开局准备或某回合失败、超出回合上限；取值同
             *         `start_session`/`step_session`。
             */
            LoopResult<GameOutcome> play_game(
                const std::string &first_player, int hand = -1);

        private:
            GameContext &m_ctx;   /**< 对局上下文（引用，非拥有）。 */
            DecisionSource &m_ai; /**< 决策源（引用，非拥有）。 */
        };

        inline std::size_t SessionQuery::alive_count(const GameContext &ctx)
        {
            return ctx.entities->size();
        }

        inline bool SessionQuery::identity_over(const GameContext &ctx)
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

        inline bool SessionQuery::session_over(const GameContext &ctx)
        {
            if (mode_of(ctx) == GameMode::Brawl)
                return ctx.entities->size() <= 1;  // 乱斗口径逐字不变
            return SessionQuery::identity_over(ctx);
        }

        inline WinCamp SessionQuery::session_camp(const GameContext &ctx)
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

        inline std::string SessionQuery::session_winner(const GameContext &ctx)
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

        inline std::string SessionQuery::next_after_seat(
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

        inline void GameSetup::deal_initial_hands(int count)
        {
            for (const auto &ent : *m_ctx.entities)
                StateOps(m_ctx).apply_draw(ent->get_id(), count);
        }

        inline void GameSetup::prepare_game(int hand)
        {
            m_ctx.cards->build_deck(*m_ctx.catalog);
            if (m_ctx.rng)
                m_ctx.cards->shuffle_draw(*m_ctx.rng);
            deal_initial_hands(hand);
        }

        inline LoopResult<void> GameSetup::start_session(
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

        inline LoopResult<void> GameLoop::step_session(
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

        inline LoopResult<GameOutcome> GameLoop::play_game(
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

        /**
         * @brief  存活玩家数（兼容转发）。
         * @details 等价于 `SessionQuery::alive_count(ctx)`。
         * @param[in] ctx 只读上下文。
         * @return 实体容器中的实体数量。
         */
        inline std::size_t alive_count(const GameContext &ctx)
        {
            return SessionQuery::alive_count(ctx);
        }

        /**
         * @brief  身份局是否终局（兼容转发）。
         * @details 等价于 `SessionQuery::identity_over(ctx)`。
         * @param[in] ctx 只读上下文。
         * @return 空场或无可判定的存活主公/敌对时返回 true。
         */
        inline bool identity_over(const GameContext &ctx)
        {
            return SessionQuery::identity_over(ctx);
        }

        /**
         * @brief  会话是否已结束（兼容转发）。
         * @details 等价于 `SessionQuery::session_over(ctx)`。
         * @param[in] ctx 只读上下文。
         * @return 已终局时为 true。
         */
        inline bool session_over(const GameContext &ctx)
        {
            return SessionQuery::session_over(ctx);
        }

        /**
         * @brief  会话胜利阵营（兼容转发）。
         * @details 等价于 `SessionQuery::session_camp(ctx)`。
         * @param[in] ctx 只读上下文。
         * @return 未结束或乱斗返回 `None`；否则为终局阵营。
         */
        inline WinCamp session_camp(const GameContext &ctx)
        {
            return SessionQuery::session_camp(ctx);
        }

        /**
         * @brief  会话胜者/阵营代表 id（兼容转发）。
         * @details 等价于 `SessionQuery::session_winner(ctx)`。
         * @param[in] ctx 只读上下文。
         * @return 乱斗 = 唯一存活者 id，否则按阵营口径；无则空串。
         */
        inline std::string session_winner(const GameContext &ctx)
        {
            return SessionQuery::session_winner(ctx);
        }

        /**
         * @brief  座位严格大于 `seat` 的第一个存活者（兼容转发）。
         * @details 等价于 `SessionQuery::next_after_seat(ctx, seat)`。
         * @param[in] ctx  只读上下文。
         * @param[in] seat 参照座位号。
         * @return 下一个存活者 id；容器为空时返回空串。
         */
        inline std::string next_after_seat(const GameContext &ctx, int seat)
        {
            return SessionQuery::next_after_seat(ctx, seat);
        }

        /**
         * @brief  每名存活玩家发 `count` 张初始手牌（兼容转发）。
         * @details 等价于 `GameSetup(ctx).deal_initial_hands(count)`。
         * @param[in,out] ctx   对局上下文。
         * @param[in]     count 每人发牌数。
         */
        inline void deal_initial_hands(GameContext &ctx, int count)
        {
            GameSetup(ctx).deal_initial_hands(count);
        }

        /**
         * @brief  开局准备（兼容转发）。
         * @details 等价于 `GameSetup(ctx).prepare_game(hand)`。
         * @param[in,out] ctx  对局上下文。
         * @param[in]     hand 每名玩家初始手牌数。
         */
        inline void prepare_game(GameContext &ctx, int hand)
        {
            GameSetup(ctx).prepare_game(hand);
        }

        /**
         * @brief  开新局（兼容转发）。
         * @details 等价于 `GameSetup(ctx).start_session(session, first_player, hand)`。
         * @param[in,out] ctx          对局上下文。
         * @param[out]    session      会话进度。
         * @param[in]     first_player 首位回合角色 id。
         * @param[in]     hand         每名玩家初始手牌数；`< 0` 时取规则配置值。
         * @return 结算结果。
         */
        inline LoopResult<void> start_session(
            GameContext &ctx, GameSession &session, const std::string &first_player,
            int hand = -1)
        {
            return GameSetup(ctx).start_session(session, first_player, hand);
        }

        /**
         * @brief  执行会话的下一个回合（兼容转发）。
         * @details 等价于 `GameLoop(ctx, ai).step_session(session, root)`。
         * @param[in,out] ctx     对局上下文。
         * @param[in,out] ai      决策源。
         * @param[in,out] session 会话进度。
         * @param[out]    root    非空时在 `TurnFailed` 分支写入回合根因。
         * @return 结算结果。
         */
        inline LoopResult<void> step_session(
            GameContext &ctx, DecisionSource &ai, GameSession &session,
            TurnError *root = nullptr)
        {
            return GameLoop(ctx, ai).step_session(session, root);
        }

        /**
         * @brief  主循环（兼容转发）。
         * @details 等价于 `GameLoop(ctx, ai).play_game(first_player, hand)`。
         * @param[in,out] ctx          对局上下文。
         * @param[in,out] ai           决策源。
         * @param[in]     first_player 首位回合角色 id。
         * @param[in]     hand         每名玩家初始手牌数；`< 0` 时取规则配置值。
         * @return 结算结果。
         */
        inline LoopResult<GameOutcome> play_game(
            GameContext &ctx, DecisionSource &ai, const std::string &first_player,
            int hand = -1)
        {
            return GameLoop(ctx, ai).play_game(first_player, hand);
        }
    }
}

#endif  // INCLUDE_TKW_GAME_LOOP_HPP
