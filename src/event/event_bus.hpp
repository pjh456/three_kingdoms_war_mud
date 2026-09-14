/**
 * @file   event_bus.hpp
 * @brief  精确类型事件总线与 RAII 订阅句柄。
 * @details 按 `typeid` 精确分发事件；订阅者以 `Handler` 表达过滤与行为，
 *          发布时按优先级与注册顺序执行，并支持停止传播。
 * @ingroup tkw_event
 */

#ifndef INCLUDE_TKW_EVENT_BUS_HPP
#define INCLUDE_TKW_EVENT_BUS_HPP

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <vector>

#include "event/event.hpp"
#include "event/handler.hpp"
#include "util/macro.hpp"

namespace tkw
{
    /**
     * @brief  精确类型事件总线。
     * @details 接收 `Handler`（filter + priority + action），`publish` 时按
     *          (priority, 注册顺序) 依次执行，并支持停止传播。
     * @tparam T 基础事件类型；只接受派生自 `T` 的事件订阅与发布。
     * @note   `subscribe` 返回 RAII 句柄，句柄析构或 `reset` 即退订；推荐「一场战斗
     *         一个总线」，会话结束时整组句柄随之销毁，handler 全部自动卸载。
     * @warning handler 不得缓存事件引用，`publish` 返回后该引用即失效。
     */
    template <typename T>
    class CommonEventBus
    {
    public:
        /** @brief 事件类型的运行时键。 */
        using Key = std::type_index;

        /**
         * @brief  订阅句柄（RAII）。
         * @details 析构或 `reset` 时从总线退订对应订阅；可移动，不可拷贝。
         * @post   默认构造或已移动出的句柄为未注册态，`active()` 返回 `false`。
         */
        class Handle
        {
            friend class CommonEventBus;

        public:
            /** @brief 构造未注册的空句柄；`active()` 为 `false`。 */
            Handle() = default;

            /**
             * @brief 移动构造：接管源句柄的订阅，源退化为未注册态。
             * @param[in,out] o 源句柄；构造后 `o.active()` 为 `false`。
             */
            Handle(Handle &&o) noexcept { *this = std::move(o); }

            /**
             * @brief  移动赋值：先退订自身订阅，再接管 `o` 的订阅。
             * @param[in,out] o 源句柄；赋值后 `o.active()` 为 `false`。
             * @return 自身引用。
             */
            Handle &operator=(Handle &&o) noexcept
            {
                if (this != &o)
                {
                    reset();
                    m_bus = o.m_bus;
                    m_key = o.m_key;
                    m_seq = o.m_seq;
                    o.m_bus = nullptr;  // 移交所有权：源退化为未注册态，析构不再退订
                }
                return *this;
            }

            // 拷贝构造已删除：一条订阅由单个句柄独占。
            Handle(const Handle &) = delete;
            // 拷贝赋值已删除：一条订阅由单个句柄独占。
            Handle &operator=(const Handle &) = delete;

            /** @brief 析构：若仍注册则退订。 */
            ~Handle() { reset(); }

            /**
             * @brief  退订并重置为未注册态。
             * @post   本句柄 `active()` 为 `false`；重复调用无副作用。
             */
            void reset() noexcept
            {
                if (m_bus == nullptr)
                    return;
                m_bus->drop(m_key, m_seq);
                m_bus = nullptr;
                m_seq = 0;
            }

            /**
             * @brief  查询是否仍持有订阅。
             * @return `true` 表示已注册；`false` 表示空句柄或已退订。
             */
            bool active() const noexcept { return m_bus != nullptr; }

        private:
            Handle(CommonEventBus *bus, Key key, std::uint64_t seq) :
                m_bus(bus), m_key(key), m_seq(seq) {}

            CommonEventBus *m_bus = nullptr;
            Key m_key{typeid(void)};
            std::uint64_t m_seq = 0;
        };

    private:
        /** @brief 已类型擦除的 handler 槽位。 */
        struct Slot
        {
            int priority = 100;
            std::uint64_t seq = 0;
            std::function<bool(const T &)> match;  // 空 = 关心全部
            std::function<bool(T &)> invoke;       // 返回 false = 停止传播
        };

        using ListenerTable = std::unordered_map<Key, std::vector<Slot>>;

        ListenerTable listeners;
        std::uint64_t m_sequence = 0;  /**< 已分配的最大事件序号 */
        std::uint64_t m_next_slot = 0; /**< 注册序号（同优先级的稳定次序） */

        void drop(Key key, std::uint64_t seq)
        {
            auto it = listeners.find(key);
            if (it == listeners.end())
                return;
            auto &slots = it->second;
            slots.erase(
                std::remove_if(slots.begin(), slots.end(),
                                [seq](const Slot &slot) { return slot.seq == seq; }),
                slots.end());
            if (slots.empty())
                listeners.erase(it);
        }

    public:
        // 默认构造空总线；`DEFAULT_CONSTRUCTOR` 同时生成拷贝/移动语义。
        DEFAULT_CONSTRUCTOR(CommonEventBus)

    public:
        /**
         * @brief  返回当前已分配的最大事件序号。
         * @return 单调递增的事件序号；尚未发布过事件时为 0。
         */
        std::uint64_t sequence() const noexcept { return m_sequence; }

        /**
         * @brief  注册一个 handler。
         * @param[in] handler 处理单元；按值传入，总线持有其拷贝。
         * @tparam U 事件类型；须派生自 `T`。
         * @return 订阅句柄：持有期间生效，析构或 `reset` 即退订。
         * @post   新订阅仅在后续 `publish` 中生效。
         */
        template <typename U>
            requires(std::is_base_of_v<T, U>)
        Handle subscribe(Handler<U> handler)
        {
            const Key key{typeid(U)};
            const std::uint64_t seq = ++m_next_slot;
            Slot slot;
            slot.priority = static_cast<int>(handler.priority());
            slot.seq = seq;
            if (handler.has_filter())
                slot.match = [h = handler](const T &base)
                { return h.matches(static_cast<const U &>(base)); };
            slot.invoke = [h = std::move(handler)](T &base)
            {
                HandlerContext<U> ctx{static_cast<U &>(base)};
                h.invoke(ctx);
                return !ctx.stop;
            };
            listeners[key].push_back(std::move(slot));
            return Handle(this, key, seq);
        }

        /**
         * @brief  发布事件：分配全局序号后按 (priority, 注册顺序) 执行命中 handler。
         * @param[in,out] event 待发布事件；总线会更新其序号。
         * @tparam U 事件类型；须派生自 `T` 且可被写入序号。
         * @pre     `event` 非空。
         * @note    分发基于监听表快照：分发期间的新注册不影响本轮，退订对本轮已排队
         *          的 handler 不再生效；handler 内可再次 `publish`（如荆棘反弹）。
         * @warning handler 不得缓存 `event` 的引用，`publish` 返回后即失效。
         */
        template <typename U>
            requires(
                std::is_base_of_v<T, U> &&
                requires(T &t) { t.set_sequence(std::uint64_t{}); })
        void publish(const std::shared_ptr<U> &event)
        {
            event->set_sequence(++m_sequence);
            auto it = listeners.find(typeid(U));
            if (it == listeners.end())
                return;
            auto slots = it->second;
            std::stable_sort(slots.begin(), slots.end(),
                             [](const Slot &a, const Slot &b) { return a.priority < b.priority; });
            for (auto &slot : slots)
            {
                if (slot.match && !slot.match(*event))
                    continue;
                if (!slot.invoke(*event))
                    break;
            }
        }
    };

    /** @brief 面向 `Event` 基类的默认总线类型。 */
    using EventBus = CommonEventBus<Event>;
}

#endif  // INCLUDE_TKW_EVENT_BUS_HPP
