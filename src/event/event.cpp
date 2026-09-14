/**
 * @file   event.cpp
 * @brief  事件基类 `Event` 的虚表锚点与类型标签定义。
 * @details 外移 `type_tag()` 使 `Event` 获得 vtable key function，虚表与类型信息
 *          仅在本文译单元发射一次。
 * @ingroup tkw_event
 */

#include "event/event.hpp"

#include <string_view>

namespace tkw
{
    std::string_view Event::type_tag() const noexcept
    {
        return "Event";
    }
}
