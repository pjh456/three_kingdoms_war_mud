/**
 * @file   log_scroll.hpp
 * @brief  日志滚动纯状态机：跟随/锚定位置与日志键位消费规则（无 FTXUI）。
 * @details 只描述「视口在哪里、按键是否被日志接管」，不含终端读写与渲染；由
 *          FTXUI 壳层读取 `ratio` 放进视口，并把按键映射成 `LogKey` 后调用
 *          本层判定。
 * @ingroup tkw_tui
 */
#ifndef INCLUDE_TKW_TUI_LOG_SCROLL_HPP
#define INCLUDE_TKW_TUI_LOG_SCROLL_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace tkw
{
    namespace tui
    {
        /** @brief 日志翻页步长（行）。 */
        inline constexpr int kLogScrollPage = 10;

        /**
         * @class LogScroll
         * @brief 日志视口滚动状态：贴尾标志 + 非跟随时锚定的绝对行下标。
         * @note  非跟随时 `anchor` 为绝对行下标（0 = 最早行），新日志到达不改变
         *        窗口；跟随时 `anchor` 无意义。行数变化由调用方以最新 `total`
         *        传回各方法。
         */
        struct LogScroll
        {
            bool follow = true;     /**< 是否贴尾（视口跟随最新行）。 */
            std::size_t anchor = 0; /**< 非跟随时锚定行下标（0 = 顶）。 */

            /**
             * @brief  视口相对位置。
             * @param[in] total 当前日志总行数。
             * @return 跟随或行数 ≤ 1 时恒 1；否则锚定行下标占总行数的比例。
             */
            float ratio(std::size_t total) const noexcept;

            /**
             * @brief  按行滚动视口。
             * @param[in] delta 正数向尾部、负数向顶部移动的行数。
             * @param[in] total 当前日志总行数。
             * @post  `anchor` 落在 `[0, total - 1]`；滚到尾部时 `follow` 恢复为真。
             * @note  贴尾时以最后一行为起点，非跟随时以锚定行为起点；两端钳位，
             *        滚到尾部恢复跟随。`total == 0` 时无操作。
             */
            void scroll(int delta, std::size_t total) noexcept;

            /**
             * @brief  回到最新行并恢复跟随（End）。
             * @param[in] total 当前日志总行数；0 时锚到 0 并保持跟随。
             */
            void to_tail(std::size_t total) noexcept;

            /**
             * @brief  跳到最早行并停止跟随（Home）。
             * @note   顶部恒为下标 0；参数 `total` 保留与 `to_tail` 对称，本实现
             *         不使用。
             */
            void to_top(std::size_t /*total*/) noexcept;
        };

        /** @brief 日志键位映射：仅描述语义，不绑定具体 UI 事件类型。 */
        enum class LogKey : std::uint8_t
        {
            None,     /**< 非日志键（回落命令输入）。 */
            PageUp,   /**< 向顶部翻页。 */
            PageDown, /**< 向尾部翻页。 */
            Home,     /**< 跳到最早行。 */
            End,      /**< 回到最新行。 */
        };

        /**
         * @brief  非面板态的日志键路由：按键与命令输入是否为空决定是否消费。
         * @param[in]     key         本次按键映射。
         * @param[in]     input_empty 命令输入缓冲是否为空。
         * @param[in,out] scroll      日志滚动状态（就地更新）。
         * @param[in]     total       当前日志总行数。
         * @return true = 已消费（事件不再下发）；false = 落回命令输入。
         * @retval true  键已被日志面板接管。
         * @retval false 键未接管，交由命令输入处理。
         * @note   `PageUp`/`PageDown` 无输入门控；`Home`/`End` 仅命令输入为空时
         *         消费，避免抢占单行编辑的光标键。
         */
        bool handle_log_key(LogKey key, bool input_empty, LogScroll &scroll,
                            std::size_t total) noexcept;
    }  // namespace tui
}  // namespace tkw

#endif  // INCLUDE_TKW_TUI_LOG_SCROLL_HPP
