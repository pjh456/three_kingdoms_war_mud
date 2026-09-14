/**
 * @file counter.hpp
 * @brief 无懈可击：抵消一张锦囊牌对一名角色产生的效果。
 * @details 规则简化实现：
 *          - 从推导起点（锦囊使用者非空 → 该玩家；空 → 目标集合首位，即延时
 *            锦囊判定窗口的被判定玩家）起，按座位序轮询「是否出无懈」；
 *          - 每出一张无懈翻转「是否被抵消」状态；一整轮无人出则结算；
 *          - 最后状态 = 出无懈次数的奇偶（链式相抵），true = 被抵消。
 * @note 只抵消锦囊牌（type == Trick），基本牌（杀/闪/桃）不可无懈。
 * @ingroup tkw_game_resolve
 */

#ifndef INCLUDE_TKW_GAME_COUNTER_HPP
#define INCLUDE_TKW_GAME_COUNTER_HPP

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "card/catalog.hpp"
#include "card/def.hpp"
#include "card/manager.hpp"
#include "game/core/context.hpp"
#include "game/core/decision.hpp"
#include "game/core/effect.hpp"
#include "game/core/state.hpp"
#include "util/types.hpp"

namespace tkw
{
    namespace game
    {
        /**
         * @brief 手牌中是否有可作无懈的牌（数据标记 counter）。
         * @param[in] ctx    只读上下文。
         * @param[in] player 被查询的实体 id。
         * @return 含可作无懈的牌时为 true；否则 false。
         * @retval true  手牌中至少一张卡定义带无懈标记。
         * @retval false 目录为空、手牌为空或无此标记。
         */
        inline bool has_counter_card(const GameContext &ctx, const std::string &player)
        {
            return any_hand_card_matching(
                ctx, player,
                [](const card::CardDef &def) { return is_counter_def(def); });
        }

        /**
         * @brief 消费一张无懈牌。
         * @param[in] ctx         对局上下文。
         * @param[in] player      打出无懈的实体 id。
         * @param[in] instance_id 选中的手牌 instance_id。
         * @return 成功消费时为 true；否则 false。
         * @retval true  该牌确为无懈，已移出手牌、弃置并发响应语义的
         *               `CardDiscarded` 事件。
         * @retval false 所选牌不存在或定义无无懈标记；状态不变。
         * @post 返回 false 时不对该玩家状态做任何改动。
         */
        inline bool consume_counter(
            GameContext &ctx, const std::string &player,
            const std::string &instance_id)
        {
            return consume_hand_card_matching(
                       ctx, player, instance_id,
                       [](const card::CardDef &def, const card::Card &)
                       { return is_counter_def(def); },
                       DiscardKind::Response)
                .is_some();
        }

        /**
         * @brief 生成从 `start` 开始环绕的座位序。
         * @param[in] ctx   只读上下文。
         * @param[in] start 环绕起点实体 id。
         * @return 从 `start` 起按座位环绕的实体 id 列表；`start` 不在场时为空。
         * @note 只读：不改变任何状态；无懈轮询起点由调用方按规则选定。
         */
        inline std::vector<std::string> seat_order_from(
            const GameContext &ctx, const std::string &start)
        {
            return ctx.entities->order_from(start);
        }

        /**
         * @brief 无懈窗口事实：一张锦囊对一组目标的一次结算窗口。
         * @note `trick_user` 为空表示延时锦囊判定窗口（使用者不随牌记录，窗口主体
         *       为被判定玩家）；`targets` 为受影响目标集合（判定窗口一人），其首位
         *       在 `trick_user` 为空时充作轮询起点。
         */
        struct CounterWindow
        {
            const card::CardDef *trick = nullptr; /**< 被结算锦囊定义。 */
            std::string trick_user;               /**< 使用者；空 = 延时判定窗口。 */
            std::vector<std::string> targets;     /**< 目标集合（判定窗口一人）。 */

            class Builder; /**< 链式构造器；定义见下。 */
        };

        /**
         * @brief `CounterWindow` 的链式构造器（可选字段按需设置）。
         * @details 每个设置方法名与所设字段同名：调用什么就是设置什么。
         */
        class CounterWindow::Builder
        {
        public:
            /**
             * @brief  设置被结算锦囊定义。
             * @param[in] trick 锦囊定义指针；须比产出窗口存活更久。
             * @return 本构造器，供链式调用。
             */
            Builder &trick(const card::CardDef *trick)
            {
                m_window.trick = trick;
                return *this;
            }

            /**
             * @brief  设置使用者。
             * @param[in] trick_user 使用者实体 id；空串 = 延时判定窗口。
             * @return 本构造器，供链式调用。
             */
            Builder &trick_user(std::string trick_user)
            {
                m_window.trick_user = std::move(trick_user);
                return *this;
            }

            /**
             * @brief  设置目标集合。
             * @param[in] targets 受影响目标 id 列表。
             * @return 本构造器，供链式调用。
             */
            Builder &targets(std::vector<std::string> targets)
            {
                m_window.targets = std::move(targets);
                return *this;
            }

            /**
             * @brief  产出组装好的无懈窗口。
             * @return 组装完成的值。
             */
            CounterWindow build() const { return m_window; }

        private:
            CounterWindow m_window; /**< 组装中的值。 */
        };

        /**
         * @brief 无懈结算操作类：以对局上下文与决策源为依赖。
         * @details 把「按座位序轮询无懈 → 奇偶相抵 → 返回是否被抵消」的窗口收敛为
         *          成员函数。
         * @warning 本类**不拥有** `ctx`/`ai`：二者须比本对象存活更久，不得跨局复用。
         * @see   CounterWindow
         */
        class CounterResolver
        {
        public:
            /**
             * @brief  绑定对局上下文与决策源。
             * @param[in,out] ctx 对局上下文；本对象只持引用。
             * @param[in,out] ai  决策源；逐玩家询问是否出无懈。
             */
            CounterResolver(GameContext &ctx, DecisionSource &ai)
                : m_ctx(ctx), m_ai(ai)
            {
            }

            /**
             * @brief  结算一次无懈窗口（链式）。
             * @param[in] window 窗口事实：锦囊定义、使用者、目标集合。
             * @return 该目标是否被无懈抵消。
             * @retval true  本窗打出奇数张无懈，效果被抵消。
             * @retval false 打出偶数张（含 0 张），效果照常结算。
             * @pre   `window.trick` 非空，`window.targets` 非空（实现取 `front()`
             *        作为 `trick_user` 为空时的回落起点）。
             * @post 本窗打出的无懈均已被消费（进入弃牌堆）；链状态不跨窗保留。
             * @note 窗口粒度 = 每个受影响目标一次（调用方按目标调用）：一张锦囊
             *       可开多个独立窗口，每个目标窗口各自出奇数张无懈才抵消该目标。
             */
            bool resolve_nullification(const CounterWindow &window);

        private:
            /**
             * @brief  询问某玩家是否打出无懈（有牌且决定出则消费）。
             * @param[in] player          被询问的实体 id。
             * @param[in] window          当前窗口事实。
             * @param[in] counter_played  本窗此前已打出的无懈张数（公开链状态）。
             * @return 本玩家是否打出了无懈。
             * @retval true  已校验并消费一张无懈。
             * @retval false 无无懈牌、决策源放弃或选择非法；状态不变。
             */
            bool try_play_counter(
                const std::string &player, const CounterWindow &window,
                int counter_played);

            GameContext &m_ctx;   /**< 对局上下文（引用，非拥有）。 */
            DecisionSource &m_ai; /**< 决策源（引用，非拥有）。 */
        };

        inline bool CounterResolver::try_play_counter(
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

        inline bool CounterResolver::resolve_nullification(
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

#endif  // INCLUDE_TKW_GAME_COUNTER_HPP
