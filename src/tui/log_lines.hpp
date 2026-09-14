/**
 * @file   log_lines.hpp
 * @brief  TUI 事件日志收集：订阅 7 类事件写入环形缓冲。
 * @details 复用 `cli/log_lines.hpp` 的纯格式化器（单一事实源）；本类只管缓冲与
 *          订阅句柄生命周期，不做任何字符串构造。
 * @ingroup tkw_tui
 */
#ifndef INCLUDE_TKW_TUI_LOG_LINES_HPP
#define INCLUDE_TKW_TUI_LOG_LINES_HPP

#include <cstddef>
#include <deque>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "cli/log_lines.hpp"
#include "event/event_bus.hpp"
#include "game/flow/table.hpp"

namespace tkw
{
    namespace tui
    {
        /**
         * @class LogBuffer
         * @brief 事件日志环形缓冲：订阅 7 类日志事件，超容量丢弃最旧行。
         * @note  句柄生命周期必须 ⊆ Game/总线生命周期：替换 Game 或销毁 Game 前
         *        必须先 `unbind()`，否则遗留句柄会向已释放总线退订。
         * @warning 不可拷贝：订阅句柄唯一持有于本对象。
         */
        class LogBuffer
        {
        public:
            /**
             * @brief  构造指定容量的日志缓冲。
             * @param[in] cap 最大保留行数；超出时从最旧行开始丢弃。
             */
            explicit LogBuffer(std::size_t cap = 256) : m_cap(cap) {}

            LogBuffer(const LogBuffer &) = delete;            /**< 不可拷贝。 */
            LogBuffer &operator=(const LogBuffer &) = delete; /**< 不可拷贝赋值。 */

            /**
             * @brief  订阅 `game` 的 7 类日志事件；重复调用先自动退订旧订阅。
             * @param[in] game   本局运行时；其生命周期必须长于本缓冲的订阅期。
             * @param[in] humans 真人座位 id；非空时摸牌牌名只对真人实体可见，
             *                   其余实体回落隐藏占位。空（默认）表示全可见，供
             *                   全 AI 对局与既有调用方保持原行为。
             * @pre   `game` 生命周期覆盖本次订阅期。
             * @note  可见性口径与 CLI 事件日志同谓词同占位，单一事实源。
             */
            void bind(tkw::game::Game &game,
                      const std::vector<std::string> &humans = {});

            /** @brief 退订全部句柄；Game 析构/覆盖前必须调用。 */
            void unbind() noexcept { m_handles.clear(); }

            /** @brief 追加一行；超过容量时丢弃最旧行。
             * @param[in] line 要追加的文本行。 */
            void push(std::string line);

            /** @brief 清空缓冲内容（不影响订阅）。 */
            void clear() { m_lines.clear(); }

            /** @brief 只读行序列。
             * @return 引用指向内部环形缓冲，生命周期同本对象。 */
            const std::deque<std::string> &lines() const noexcept
            {
                return m_lines;
            }

            /** @brief 缓冲容量上限。
             * @return 最大保留行数。 */
            std::size_t capacity() const noexcept { return m_cap; }

        private:
            std::deque<std::string> m_lines;
            std::size_t m_cap = 256;
            std::vector<tkw::EventBus::Handle> m_handles;
        };
    }  // namespace tui
}  // namespace tkw

#endif  // INCLUDE_TKW_TUI_LOG_LINES_HPP
