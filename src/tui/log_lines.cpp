/**
 * @file   log_lines.cpp
 * @brief  TUI 事件日志收集实现：订阅 7 类事件写入环形缓冲。
 * @ingroup tkw_tui
 */
#include "tui/log_lines.hpp"

#include <cstddef>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace tkw
{
    namespace tui
    {
        void LogBuffer::bind(tkw::game::Game &game,
                             const std::vector<std::string> &humans)
        {
            unbind();
            const std::set<std::string> visible(humans.begin(), humans.end());
            m_handles = tkw::cli::detail::subscribe_event_log_to(
                game, [this](std::string line) { push(std::move(line)); },
                [visible](const std::string &entity)
                { return visible.empty() || visible.count(entity) > 0; });
        }

        void LogBuffer::push(std::string line)
        {
            m_lines.push_back(std::move(line));
            while (m_lines.size() > m_cap)
                m_lines.pop_front();
        }
    }  // namespace tui
}  // namespace tkw
