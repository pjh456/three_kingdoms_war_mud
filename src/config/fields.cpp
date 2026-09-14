/**
 * @file   fields.cpp
 * @brief  JSON 节点上的类型化字段提取函数的定义。
 * @ingroup tkw_config
 */

#include "config/fields.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace tkw
{
    namespace config
    {
        std::string field_path(std::string_view path, std::string_view key)
        {
            if (path.empty())
                return std::string(key);
            return std::string(path) + "." + std::string(key);
        }

        std::string container_path(std::string_view path)
        {
            return path.empty() ? std::string("root") : std::string(path);
        }

        ConfigResult<std::int64_t> require_int(
            const json::Json &obj, std::string_view key, std::string_view path)
        {
            const auto *o = obj.try_as_object();
            if (!o)
                return fail<std::int64_t>(
                    ConfigErrorKind::TypeMismatch, container_path(path));
            if (!o->contains(key))
                return fail<std::int64_t>(
                    ConfigErrorKind::MissingField, field_path(path, key));
            const json::Json &v = (*o)[key];
            auto r = v.try_as_int();
            if (!r)
                return fail<std::int64_t>(
                    ConfigErrorKind::TypeMismatch, field_path(path, key));
            return ConfigResult<std::int64_t>::Ok(*r);
        }

        ConfigResult<std::string> require_string(
            const json::Json &obj, std::string_view key, std::string_view path)
        {
            const auto *o = obj.try_as_object();
            if (!o)
                return fail<std::string>(
                    ConfigErrorKind::TypeMismatch, container_path(path));
            if (!o->contains(key))
                return fail<std::string>(
                    ConfigErrorKind::MissingField, field_path(path, key));
            const json::Json &v = (*o)[key];
            auto r = v.try_as_string();
            if (!r)
                return fail<std::string>(
                    ConfigErrorKind::TypeMismatch, field_path(path, key));
            return ConfigResult<std::string>::Ok(std::string(*r));
        }

        ConfigResult<bool> require_bool(
            const json::Json &obj, std::string_view key, std::string_view path)
        {
            const auto *o = obj.try_as_object();
            if (!o)
                return fail<bool>(ConfigErrorKind::TypeMismatch, container_path(path));
            if (!o->contains(key))
                return fail<bool>(ConfigErrorKind::MissingField, field_path(path, key));
            const json::Json &v = (*o)[key];
            auto r = v.try_as_boolean();
            if (!r)
                return fail<bool>(ConfigErrorKind::TypeMismatch, field_path(path, key));
            return ConfigResult<bool>::Ok(*r);
        }

        ConfigResult<const json::Json *> require_object(
            const json::Json &obj, std::string_view key, std::string_view path)
        {
            const auto *o = obj.try_as_object();
            if (!o)
                return fail<const json::Json *>(
                    ConfigErrorKind::TypeMismatch, container_path(path));
            if (!o->contains(key))
                return fail<const json::Json *>(
                    ConfigErrorKind::MissingField, field_path(path, key));
            const json::Json &v = (*o)[key];
            if (!v.try_as_object())
                return fail<const json::Json *>(
                    ConfigErrorKind::TypeMismatch, field_path(path, key));
            return ConfigResult<const json::Json *>::Ok(&v);
        }

        ConfigResult<const json::Array *> require_array(
            const json::Json &obj, std::string_view key, std::string_view path)
        {
            const auto *o = obj.try_as_object();
            if (!o)
                return fail<const json::Array *>(
                    ConfigErrorKind::TypeMismatch, container_path(path));
            if (!o->contains(key))
                return fail<const json::Array *>(
                    ConfigErrorKind::MissingField, field_path(path, key));
            const json::Json &v = (*o)[key];
            const auto *arr = v.try_as_array();
            if (!arr)
                return fail<const json::Array *>(
                    ConfigErrorKind::TypeMismatch, field_path(path, key));
            return ConfigResult<const json::Array *>::Ok(arr);
        }

        ConfigResult<std::int64_t> opt_int(
            const json::Json &obj,
            std::string_view key,
            std::int64_t def,
            std::string_view path)
        {
            const auto *o = obj.try_as_object();
            if (!o)
                return fail<std::int64_t>(
                    ConfigErrorKind::TypeMismatch, container_path(path));
            if (!o->contains(key))
                return ConfigResult<std::int64_t>::Ok(def);
            const json::Json &v = (*o)[key];
            auto r = v.try_as_int();
            if (!r)
                return fail<std::int64_t>(
                    ConfigErrorKind::TypeMismatch, field_path(path, key));
            return ConfigResult<std::int64_t>::Ok(*r);
        }

        ConfigResult<int> opt_int_range(
            const json::Json &obj,
            std::string_view key,
            int def,
            std::string_view path)
        {
            auto r = opt_int(obj, key, def, path);
            if (r.is_err())
                return ConfigResult<int>::Err(r.unwrap_err());
            const std::int64_t v = r.unwrap();
            if (v < std::numeric_limits<int>::min() ||
                v > std::numeric_limits<int>::max())
                return fail<int>(ConfigErrorKind::InvalidValue, field_path(path, key));
            return ConfigResult<int>::Ok(static_cast<int>(v));
        }

        ConfigResult<std::string> opt_string(
            const json::Json &obj,
            std::string_view key,
            std::string_view def,
            std::string_view path)
        {
            const auto *o = obj.try_as_object();
            if (!o)
                return fail<std::string>(
                    ConfigErrorKind::TypeMismatch, container_path(path));
            if (!o->contains(key))
                return ConfigResult<std::string>::Ok(std::string(def));
            const json::Json &v = (*o)[key];
            auto r = v.try_as_string();
            if (!r)
                return fail<std::string>(
                    ConfigErrorKind::TypeMismatch, field_path(path, key));
            return ConfigResult<std::string>::Ok(std::string(*r));
        }

        ConfigResult<bool> opt_bool(
            const json::Json &obj,
            std::string_view key,
            bool def,
            std::string_view path)
        {
            const auto *o = obj.try_as_object();
            if (!o)
                return fail<bool>(ConfigErrorKind::TypeMismatch, container_path(path));
            if (!o->contains(key))
                return ConfigResult<bool>::Ok(def);
            const json::Json &v = (*o)[key];
            auto r = v.try_as_boolean();
            if (!r)
                return fail<bool>(ConfigErrorKind::TypeMismatch, field_path(path, key));
            return ConfigResult<bool>::Ok(*r);
        }
    }
}
