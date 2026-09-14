/**
 * @file   macro.hpp
 * @brief  通用成员声明宏。
 * @details 用于消除值类型样板代码：五个特殊成员函数的默认声明，以及
 *          「私有字段 + 公有 getter」属性声明。
 * @ingroup tkw_util
 */

#ifndef INCLUDE_TKW_UTIL_MACRO_HPP
#define INCLUDE_TKW_UTIL_MACRO_HPP

/**
 * @brief  声明默认的拷贝/移动构造与赋值。
 * @param[in] name 目标类型名；宏使用其拼装构造函数与赋值运算符。
 */
#define DEFAULT_CONSTRUCTOR(name)            \
    name() = default;                        \
    name(const name &) = default;            \
    name(name &&) noexcept = default;        \
    name &operator=(const name &) = default; \
    name &operator=(name &&) noexcept = default;


/**
 * @brief  声明私有字段及其公有读写访问器。
 * @details 展开为私有成员 `name`，以及 `get_##name()` 的可变与常量重载；
 *          访问器为 `noexcept` 且返回引用。
 * @param[in] type 字段类型。
 * @param[in] name 字段名；访问器按 `get_<name>` 命名。
 */
#define DEFINE_ATTRIBUTE(type, name)             \
private:                                         \
    type name;                                   \
                                                 \
public:                                          \
    type &get_##name() noexcept { return name; } \
    const type &get_##name() const noexcept { return name; }

#endif  // INCLUDE_TKW_UTIL_MACRO_HPP
