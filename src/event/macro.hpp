/**
 * @file   macro.hpp
 * @brief  事件类定义的便捷宏。
 * @details `DEFINE_EVENT_START` 与 `DEFINE_EVENT_END` 成对包裹，生成派生事件类与
 *          `type_tag()` 覆盖。
 * @ingroup tkw_event
 */

#ifndef INCLUDE_TKW_EVENT_MACRO_HPP
#define INCLUDE_TKW_EVENT_MACRO_HPP

/**
 * @brief  开始定义一个事件类。
 * @details 展开为 `class <name>Event : public <parent> {`，其后接成员声明。
 * @param[in] name   事件名（不含 `Event` 后缀）；宏拼接为 `<name>Event`。
 * @param[in] parent 基类名。
 * @see   DEFINE_EVENT_END
 */
#define DEFINE_EVENT_START(name, parent) \
    class name##Event : public parent    \
    {

/**
 * @brief  结束事件类定义。
 * @details 生成 `type_tag()` 覆盖并闭合类体，标签字符串为 `<name>Event`。
 * @param[in] name 与 `DEFINE_EVENT_START` 相同的名字。
 * @see   DEFINE_EVENT_START
 */
#define DEFINE_EVENT_END(name)                                                   \
public:                                                                          \
    std::string_view type_tag() const noexcept override { return #name "Event"; } \
    }                                                                            \
    ;

#endif  // INCLUDE_TKW_EVENT_MACRO_HPP
