/**
 * @file   types.hpp
 * @brief  基础类型别名：统一表达「可能失败」与「可能缺失」。
 * @details 转发 `pjh::result` 的实现，使各层只依赖本头文件即可使用
 *          `Result`/`Option`，无需直接绑定第三方命名空间。
 * @ingroup tkw_util
 */

#ifndef INCLUDE_TKW_UTIL_TYPES_HPP
#define INCLUDE_TKW_UTIL_TYPES_HPP

#include <pjh_result.hpp>

namespace tkw
{
    /**
     * @brief  可能失败的运算结果。
     * @tparam T 成功时的值类型。
     * @tparam E 失败时的错误值类型。
     * @see    Option
     */
    template <typename T, typename E>
    using Result = pjh::result::Result<T, E>;

    /**
     * @brief  可能缺失的值。
     * @tparam T 存在时的值类型。
     * @see    Result
     */
    template <typename T>
    using Option = pjh::result::Option<T>;
}

#endif  // INCLUDE_TKW_UTIL_TYPES_HPP
