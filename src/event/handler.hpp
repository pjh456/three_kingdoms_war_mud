/**
 * @file   handler.hpp
 * @brief  事件处理器与组合子。
 * @details 定义处理优先级、handler 上下文，以及 `pipe`/`fanout`/`when` 三种组合器。
 * @ingroup tkw_event
 */

#ifndef INCLUDE_TKW_EVENT_HANDLER_HPP
#define INCLUDE_TKW_EVENT_HANDLER_HPP

#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace tkw
{
    /**
     * @brief  处理优先级：数值小者先执行；同优先级按注册顺序。
     */
    enum class HandlerPriority : int
    {
        First  = 0,   /**< 最先执行。 */
        Normal = 100, /**< 默认优先级。 */
        Last   = 200, /**< 最后执行。 */
    };

    /**
     * @brief  交给 handler action 的上下文。
     * @details 携带事件本体与传播开关。
     * @tparam EventT 事件类型；与所属 handler 的模板参数一致。
     * @note   action 置 `stop` 后，同一次 `publish` 内排在后面的 handler 不再执行。
     */
    template <typename EventT>
    struct HandlerContext
    {
        EventT &event;     /**< 当前事件；仅在本次 `publish` 期间有效。 */
        bool stop = false; /**< 置 `true` 停止同一次 `publish` 的后续 handler。 */
    };

    /**
     * @brief  针对某事件类型的最小行为单元。
     * @details filter 回答「是否关心」，action 只做一件事，priority 决定执行先后。
     * @tparam EventT 事件类型；本 handler 只接受该类型的事件。
     * @note   不依赖总线即可直接构造/调用/断言，便于独立单测；
     *         用 `pipe` / `fanout` / `when` 组合成更复杂的行为。
     */
    template <typename EventT>
    class Handler
    {
    public:
        /** @brief 事件类型别名。 */
        using Event  = EventT;
        /** @brief 过滤谓词类型。 */
        using Filter = std::function<bool(const EventT &)>;
        /** @brief 行为回调类型。 */
        using Action = std::function<void(HandlerContext<EventT> &)>;

        /** @brief 构造无 action 的空 handler；未设置 action 前不得调用 `invoke`。 */
        Handler() = default;

        /**
         * @brief  用 action 构造 handler。
         * @param[in] action   行为回调。
         * @param[in] priority 执行优先级，默认 `HandlerPriority::Normal`。
         */
        explicit Handler(Action action, HandlerPriority priority = HandlerPriority::Normal) :
            m_action(std::move(action)), m_priority(priority) {}

        /**
         * @brief  用 filter 与 action 构造 handler。
         * @param[in] filter   过滤谓词；决定本 handler 是否关心某事件。
         * @param[in] action   行为回调。
         * @param[in] priority 执行优先级，默认 `HandlerPriority::Normal`。
         */
        Handler(Filter filter, Action action, HandlerPriority priority = HandlerPriority::Normal) :
            m_filter(std::move(filter)), m_action(std::move(action)), m_priority(priority) {}

        /**
         * @brief  查询是否设置了过滤谓词。
         * @return `true` 表示设置了非空 filter。
         */
        bool has_filter() const noexcept { return static_cast<bool>(m_filter); }

        /**
         * @brief  判断本 handler 是否关心该事件。
         * @param[in] event 待判断的事件。
         * @return `true` 表示未设 filter，或 filter 命中该事件。
         */
        bool matches(const EventT &event) const { return !m_filter || m_filter(event); }

        /**
         * @brief  执行 action。
         * @param[in,out] ctx handler 上下文；action 可经 `ctx.stop` 停止传播。
         * @pre     本 handler 已设置 action。
         */
        void invoke(HandlerContext<EventT> &ctx) const { m_action(ctx); }

        /**
         * @brief  返回执行优先级。
         * @return 构造时指定的优先级值。
         */
        HandlerPriority priority() const noexcept { return m_priority; }

    private:
        Filter m_filter;  // 空 = 关心所有该类型事件
        Action m_action;
        HandlerPriority m_priority = HandlerPriority::Normal;
    };

    /**
     * @brief  管线组合：按序执行各阶段，任一阶段置 `stop` 即短路。
     * @param[in] first 首个阶段。
     * @param[in] rest  其余阶段，按给定顺序串接。
     * @tparam EventT 事件类型；所有阶段共享同一事件类型。
     * @tparam Rest   其余阶段 handler 的转发引用类型。
     * @return 组合后的单一 handler。
     * @note   用于「计算 → 校验 → 结算」这类后段依赖前段的阶段链。
     */
    template <typename EventT, typename... Rest>
    Handler<EventT> pipe(Handler<EventT> first, Rest &&...rest)
    {
        std::vector<Handler<EventT>> stages{std::move(first), std::forward<Rest>(rest)...};
        return Handler<EventT>(
            [stages = std::move(stages)](HandlerContext<EventT> &ctx) mutable
            {
                for (auto &stage : stages)
                {
                    if (!stage.matches(ctx.event))
                        continue;
                    stage.invoke(ctx);
                    if (ctx.stop)
                        break;
                }
            });
    }

    /**
     * @brief  分发组合：所有 handler 各自独立执行，互不影响，不看 `stop`。
     * @param[in] first 首个 handler。
     * @param[in] rest  其余 handler。
     * @tparam EventT 事件类型；所有 handler 共享同一事件类型。
     * @tparam Rest   其余 handler 的转发引用类型。
     * @return 组合后的单一 handler；各 handler 自带的 filter 仍生效。
     * @note   用于「受伤 → (记日志, 荆棘反弹, 连击计数)」这类独立关注点。
     */
    template <typename EventT, typename... Rest>
    Handler<EventT> fanout(Handler<EventT> first, Rest &&...rest)
    {
        std::vector<Handler<EventT>> handlers{std::move(first), std::forward<Rest>(rest)...};
        return Handler<EventT>(
            [handlers = std::move(handlers)](HandlerContext<EventT> &ctx) mutable
            {
                for (auto &handler : handlers)
                    if (handler.matches(ctx.event))
                        handler.invoke(ctx);
            });
    }

    /**
     * @brief  条件组合：仅当 `pred(event)` 为真且 `h` 自身 filter 命中时才执行 `h`。
     * @param[in] pred 外层条件谓词。
     * @param[in] h    被委托的 handler；组合体沿用其优先级。
     * @tparam EventT 事件类型。
     * @tparam Pred   谓词类型；可经 `bool(const EventT &)` 调用。
     * @return 组合后的单一 handler。
     */
    template <typename EventT, typename Pred>
    Handler<EventT> when(Pred &&pred, Handler<EventT> h)
    {
        auto outer = typename Handler<EventT>::Filter(std::forward<Pred>(pred));
        auto inner = std::make_shared<Handler<EventT>>(std::move(h));
        return Handler<EventT>(
            [outer, inner](const EventT &event) { return outer(event) && inner->matches(event); },
            [inner](HandlerContext<EventT> &ctx) { inner->invoke(ctx); },
            inner->priority());
    }
}

#endif  // INCLUDE_TKW_EVENT_HANDLER_HPP
