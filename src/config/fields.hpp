/**
 * @file   fields.hpp
 * @brief  JSON 节点上的类型化字段提取（域无关，不含任何游戏规则）。
 * @details 语义约定：require_* = 缺失/类型不符都硬失败；opt_* = 仅「缺失」允许
 *          回落默认值，类型不符仍失败（默认值不得吞掉写错类型）。
 *          path 参数是**容器自身**的路径（`""` = 顶层），用于拼装错误里的
 *          字段路径：`require_int(doc.root(), "damage", "cards[3]")` 报错时
 *          detail = `"cards[3].damage"`。
 * @ingroup tkw_config
 */

#ifndef INCLUDE_TKW_CONFIG_FIELDS_HPP
#define INCLUDE_TKW_CONFIG_FIELDS_HPP

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include <pjh_json/json.hpp>

#include "config/error.hpp"

namespace tkw
{
    namespace config
    {
        namespace json = pjh::json;

        /**
         * @brief  拼接错误消息里的字段路径。
         * @param[in] path 容器自身路径；空串表示顶层。
         * @param[in] key  字段名。
         * @return `path` 为空时返回 `key` 本身，否则返回 `path.key`。
         */
        std::string field_path(std::string_view path, std::string_view key);

        /**
         * @brief  规整容器自身路径。
         * @param[in] path 容器自身路径；空串表示顶层。
         * @return `path` 为空时返回 `root`，否则原样返回。
         */
        std::string container_path(std::string_view path);

        /**
         * @brief  构造携带字段路径错误的 `ConfigResult`。
         * @tparam T 成功时的值类型。
         * @param[in] kind   错误类别。
         * @param[in] detail 上下文文本（通常是字段路径）。
         * @return 恒为 `Err(ConfigError{kind, detail})`。
         */
        template <typename T>
        ConfigResult<T> fail(ConfigErrorKind kind, std::string detail)
        {
            return ConfigResult<T>::Err(ConfigError{kind, std::move(detail)});
        }

        /**
         * @brief  必填整型字段。缺失 → MissingField；类型不符 → TypeMismatch。
         * @param[in] obj  容器 JSON 对象。
         * @param[in] key  字段名。
         * @param[in] path 容器自身路径；空串表示顶层。
         * @return 字段值；失败时 `Err` 携带原因。
         * @retval Ok  `int64` 字段值。
         * @retval Err(ConfigErrorKind::MissingField) 字段缺失，detail 为字段路径。
         * @retval Err(ConfigErrorKind::TypeMismatch) 容器非对象或字段非整数。
         */
        ConfigResult<std::int64_t> require_int(
            const json::Json &obj, std::string_view key, std::string_view path = {});

        /**
         * @brief  必填字符串字段（拷贝出来，不依赖 Document 生命周期）。
         * @param[in] obj  容器 JSON 对象。
         * @param[in] key  字段名。
         * @param[in] path 容器自身路径；空串表示顶层。
         * @return 字段值；失败时 `Err` 携带原因。
         * @retval Ok  `std::string` 字段值。
         * @retval Err(ConfigErrorKind::MissingField) 字段缺失，detail 为字段路径。
         * @retval Err(ConfigErrorKind::TypeMismatch) 容器非对象或字段非字符串。
         */
        ConfigResult<std::string> require_string(
            const json::Json &obj, std::string_view key, std::string_view path = {});

        /**
         * @brief  必填布尔字段。
         * @param[in] obj  容器 JSON 对象。
         * @param[in] key  字段名。
         * @param[in] path 容器自身路径；空串表示顶层。
         * @return 字段值；失败时 `Err` 携带原因。
         * @retval Ok  `bool` 字段值。
         * @retval Err(ConfigErrorKind::MissingField) 字段缺失，detail 为字段路径。
         * @retval Err(ConfigErrorKind::TypeMismatch) 容器非对象或字段非布尔。
         */
        ConfigResult<bool> require_bool(
            const json::Json &obj, std::string_view key, std::string_view path = {});

        /**
         * @brief  必填对象字段。
         * @param[in] obj  容器 JSON 对象。
         * @param[in] key  字段名。
         * @param[in] path 容器自身路径；空串表示顶层。
         * @return 指向子对象的指针；失败时 `Err` 携带原因。
         * @retval Ok  指向容器内子对象的只读指针，生命周期随 `obj`。
         * @retval Err(ConfigErrorKind::MissingField) 字段缺失，detail 为字段路径。
         * @retval Err(ConfigErrorKind::TypeMismatch) 容器非对象或字段非对象。
         */
        ConfigResult<const json::Json *> require_object(
            const json::Json &obj, std::string_view key, std::string_view path = {});

        /**
         * @brief  必填数组字段。
         * @param[in] obj  容器 JSON 对象。
         * @param[in] key  字段名。
         * @param[in] path 容器自身路径；空串表示顶层。
         * @return 指向子数组的指针；失败时 `Err` 携带原因。
         * @retval Ok  指向容器内数组的只读指针，生命周期随 `obj`。
         * @retval Err(ConfigErrorKind::MissingField) 字段缺失，detail 为字段路径。
         * @retval Err(ConfigErrorKind::TypeMismatch) 容器非对象或字段非数组。
         */
        ConfigResult<const json::Array *> require_array(
            const json::Json &obj, std::string_view key, std::string_view path = {});

        /**
         * @brief  可选整型字段：缺失回落默认值；**类型不符仍失败**。
         * @param[in] obj  容器 JSON 对象。
         * @param[in] key  字段名。
         * @param[in] def  字段缺失时的默认值。
         * @param[in] path 容器自身路径；空串表示顶层。
         * @return 字段值或 `def`；失败时 `Err` 携带原因。
         * @retval Ok  字段存在则为字段值，缺失则为 `def`。
         * @retval Err(ConfigErrorKind::TypeMismatch) 容器非对象或字段非整数。
         */
        ConfigResult<std::int64_t> opt_int(
            const json::Json &obj,
            std::string_view key,
            std::int64_t def,
            std::string_view path = {});

        /**
         * @brief  可选整型字段，收窄为 int：缺失回落默认值。
         * @param[in] obj  容器 JSON 对象。
         * @param[in] key  字段名。
         * @param[in] def  字段缺失时的默认值。
         * @param[in] path 容器自身路径；空串表示顶层。
         * @return 收窄后的 `int`；失败时 `Err` 携带原因。
         * @retval Ok  字段缺失则为 `def`，否则为收窄后的字段值。
         * @retval Err(ConfigErrorKind::TypeMismatch) 容器非对象或字段非整数。
         * @retval Err(ConfigErrorKind::InvalidValue) 数值超出 `int` 可表示范围
         *         （收窄前检查，不静默截断）。
         */
        ConfigResult<int> opt_int_range(
            const json::Json &obj,
            std::string_view key,
            int def,
            std::string_view path = {});

        /**
         * @brief  可选字符串字段：缺失回落默认值；类型不符仍失败。
         * @param[in] obj  容器 JSON 对象。
         * @param[in] key  字段名。
         * @param[in] def  字段缺失时的默认值。
         * @param[in] path 容器自身路径；空串表示顶层。
         * @return 字段值或 `def`；失败时 `Err` 携带原因。
         * @retval Ok  字段存在则为字段值，缺失则为 `def`。
         * @retval Err(ConfigErrorKind::TypeMismatch) 容器非对象或字段非字符串。
         */
        ConfigResult<std::string> opt_string(
            const json::Json &obj,
            std::string_view key,
            std::string_view def,
            std::string_view path = {});

        /**
         * @brief  可选布尔字段：缺失回落默认值；类型不符仍失败。
         * @param[in] obj  容器 JSON 对象。
         * @param[in] key  字段名。
         * @param[in] def  字段缺失时的默认值。
         * @param[in] path 容器自身路径；空串表示顶层。
         * @return 字段值或 `def`；失败时 `Err` 携带原因。
         * @retval Ok  字段存在则为字段值，缺失则为 `def`。
         * @retval Err(ConfigErrorKind::TypeMismatch) 容器非对象或字段非布尔。
         */
        ConfigResult<bool> opt_bool(
            const json::Json &obj,
            std::string_view key,
            bool def,
            std::string_view path = {});

        /**
         * @brief  迭代数组字段：对每个元素调用 `f(element, item_path)`。
         * @details `item_path` 形如 `cards[3]`，可直接作为 require 系列函数的
         *          `path` 参数，拼出 `cards[3].damage` 这类完整错误路径。
         * @tparam F 返回 `ConfigResult<void>` 的回调（元素级错误经其返回传播）。
         * @param[in] obj  容器 JSON 对象。
         * @param[in] key  字段名。
         * @param[in] path 容器自身路径；空串表示顶层。
         * @param[in] f    元素回调；接收元素与 `item_path`。
         * @return 全部元素处理成功为 `Ok`；否则为首个元素错误。
         * @retval Ok  数组存在且所有元素回调均成功。
         * @retval Err(ConfigErrorKind::MissingField) 字段缺失，detail 为字段路径。
         * @retval Err(ConfigErrorKind::TypeMismatch) 容器非对象或字段非数组。
         * @retval Err 首个元素回调返回的错误原样传播。
         * @note   首个元素错误立即返回，后续元素不再处理。
         */
        template <typename F>
        ConfigResult<void> each(
            const json::Json &obj, std::string_view key, std::string_view path, F &&f)
        {
            const auto *o = obj.try_as_object();
            if (!o)
                return fail<void>(ConfigErrorKind::TypeMismatch, container_path(path));
            if (!o->contains(key))
                return fail<void>(ConfigErrorKind::MissingField, field_path(path, key));
            const json::Json &v = (*o)[key];
            const auto *arr = v.try_as_array();
            if (!arr)
                return fail<void>(ConfigErrorKind::TypeMismatch, field_path(path, key));

            const auto prefix = field_path(path, key);
            for (std::size_t i = 0; i < arr->size(); ++i)
            {
                const std::string item_path = prefix + "[" + std::to_string(i) + "]";
                auto r = f((*arr)[i], item_path);
                if (r.is_err())
                    return fail<void>(r.unwrap_err().kind, r.unwrap_err().detail);
            }
            return ConfigResult<void>::Ok();
        }
    }
}

#endif  // INCLUDE_TKW_CONFIG_FIELDS_HPP
