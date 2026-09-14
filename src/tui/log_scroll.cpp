/**
 * @file   log_scroll.cpp
 * @brief  日志滚动纯状态机实现：跟随/锚定位置与日志键位消费规则。
 * @ingroup tkw_tui
 */
#include "tui/log_scroll.hpp"

#include <algorithm>
#include <cstddef>

namespace tkw
{
    namespace tui
    {
        float LogScroll::ratio(std::size_t total) const noexcept
        {
            if (total <= 1 || follow)
                return 1.0f;
            const std::size_t a = std::min(anchor, total - 1);
            return static_cast<float>(a) /
                   static_cast<float>(total - 1);
        }

        void LogScroll::scroll(int delta, std::size_t total) noexcept
        {
            if (total == 0)
                return;

            const int last = static_cast<int>(total - 1);
            int base = follow
                           ? last
                           : static_cast<int>(std::min(anchor, total - 1));
            base = std::clamp(base + delta, 0, last);
            anchor = static_cast<std::size_t>(base);
            follow = base >= last;
        }

        void LogScroll::to_tail(std::size_t total) noexcept
        {
            anchor = total == 0 ? 0 : total - 1;
            follow = true;
        }

        void LogScroll::to_top(std::size_t /*total*/) noexcept
        {
            anchor = 0;
            follow = false;
        }

        bool handle_log_key(LogKey key, bool input_empty, LogScroll &scroll,
                            std::size_t total) noexcept
        {
            if (key == LogKey::None)
                return false;
            if (key == LogKey::PageUp)
            {
                scroll.scroll(-kLogScrollPage, total);
                return true;
            }
            if (key == LogKey::PageDown)
            {
                scroll.scroll(kLogScrollPage, total);
                return true;
            }
            if (!input_empty)
                return false;
            if (key == LogKey::Home)
            {
                scroll.to_top(total);
                return true;
            }
            scroll.to_tail(total);
            return true;
        }
    }  // namespace tui
}  // namespace tkw
