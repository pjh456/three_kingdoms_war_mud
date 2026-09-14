/**
 * @file   event.hpp
 * @brief  事件基类：携带全局序号并暴露类型标签。
 * @details 所有可发布事件都派生自 `Event`；`type_tag()` 供日志与调试输出使用。
 * @ingroup tkw_event
 */

#ifndef INCLUDE_TKW_EVENT_EVENT_HPP
#define INCLUDE_TKW_EVENT_EVENT_HPP

#include <cstdint>
#include <string_view>

namespace tkw
{
    template <typename T>
    class CommonEventBus;

    /**
     * @brief  事件基类：提供全局序号与类型标签。
     * @details 发布总线为每个事件分配单调递增序号；`type_tag()` 返回可读类型名。
     * @note   派生类须经 `CommonEventBus::publish` 发布，以取得合法序号。
     */
    class Event
    {
        friend class CommonEventBus<Event>;

    private:
        std::uint64_t m_sequence = 0; /**< 全局发布序号；发布前为 0。 */

        void set_sequence(std::uint64_t seq) noexcept { m_sequence = seq; }

    public:
        /**
         * @brief 构造事件，序号初始化为 0。
         * @post  `m_sequence` 为 0；发布后由总线赋值为全局序号。
         */
        Event() = default;

        /** @brief 虚析构：保证经基类指针销毁派生事件。 */
        virtual ~Event() = default;

        /**
         * @brief  返回事件类型标签。
         * @return 可读的类型名字符串视图；基类固定返回 `"Event"`。
         * @note   派生类经 `DEFINE_EVENT_END` 覆盖为各自的类型名。
         */
        virtual std::string_view type_tag() const noexcept { return "Event"; }

        /**
         * @brief  返回全局发布序号。
         * @return 该事件被 `CommonEventBus::publish` 时分配的单调递增序号；未发布为 0。
         */
        std::uint64_t get_sequence() const noexcept { return m_sequence; }
    };
}

#endif  // INCLUDE_TKW_EVENT_EVENT_HPP
