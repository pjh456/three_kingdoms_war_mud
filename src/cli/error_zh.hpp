/**
 * @file error_zh.hpp
 * @brief CLI 解析错误中文渲染：把框架 CliError 渲染为面向用户的中文文本。
 * @note 只渲染不改退出码：解析错误（Parse）加中文前缀 + 中文正文，运行时错误
 *       （Runtime）原样返回 what()（项目自身消息本已中文、无前缀）。批量入口与
 *       REPL 共用同一渲染，保证两条路径文案一致。
 */
#ifndef INCLUDE_TKW_CLI_ERROR_ZH_HPP
#define INCLUDE_TKW_CLI_ERROR_ZH_HPP

#include <concepts>
#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include <pjh_cli.hpp>

namespace tkw
{
    namespace cli
    {
        /** 解析错误统一前缀（替代框架英文 "Parse Error: "）。 */
        inline constexpr std::string_view kParseErrorPrefix = "参数错误: ";

        namespace detail
        {
            /** 把字符串列表按分隔符拼接；空列表返回空串。 */
            inline std::string join_items(
                const std::vector<std::string> &items, std::string_view sep)
            {
                std::string out;
                for (std::size_t i = 0; i < items.size(); ++i)
                {
                    if (i > 0)
                        out += sep;
                    out += items[i];
                }
                return out;
            }

            /**
             * @brief 框架类型名 → 中文；未命中回落原文。
             * @param expected_type 框架 TypeConversionError 的 expected_type。
             * @return "integer"→"整数"、"float"→"浮点数"、"bool" 开头→"布尔值"，
             *         其余原样，避免半英半中。
             */
            inline std::string expected_type_zh(std::string_view expected_type)
            {
                if (expected_type == "integer")
                    return "整数";
                if (expected_type == "float")
                    return "浮点数";
                if (expected_type.starts_with("bool"))
                    return "布尔值";
                return std::string(expected_type);
            }
        }  // namespace detail

        /**
         * @brief 把单个 ErrorInfo 渲染为中文正文（不含前缀）。
         * @param info 框架解析错误负载。
         * @return 对应 variant 的中文文案；未识别 variant 回落框架英文 format_error，
         *         保证上游新增分支时项目仍可编译运行。
         * @note 穷举当前 16 个 variant；随附建议/候选/取值列表以「、」分隔。
         */
        inline std::string render_parse_error_zh(const pjh::cli::ErrorInfo &info)
        {
            return std::visit(
                [&info](const auto &e) -> std::string
                {
                    using T = std::decay_t<decltype(e)>;

                    if constexpr (std::same_as<T, pjh::cli::RawMessageError>)
                        return e.message;
                    else if constexpr (std::same_as<T, pjh::cli::ParseError>)
                        return "参数解析失败: '" + e.raw_input + "'（第 " +
                               std::to_string(e.position) + " 个参数）";
                    else if constexpr (std::same_as<T, pjh::cli::UnknownOptionError>)
                    {
                        if (e.suggestions.empty())
                            return "未知选项: '" + e.option_display + "'";
                        return "未知选项: '" + e.option_display + "'；您是否要找: " +
                               detail::join_items(e.suggestions, "、");
                    }
                    else if constexpr (std::same_as<T, pjh::cli::MissingValueError>)
                        return "选项 '" + e.option_display + "' 需要一个值";
                    else if constexpr (std::same_as<
                                           T, pjh::cli::MissingRequiredOptionError>)
                        return "缺少必需选项: '" + e.option_name + "'";
                    else if constexpr (std::same_as<
                                           T, pjh::cli::MissingRequiredArgError>)
                        return "缺少必需参数: '" + e.arg_name + "'";
                    else if constexpr (std::same_as<T, pjh::cli::TypeConversionError>)
                        return "选项 '" + e.option_display + "' 的值 '" + e.raw_value +
                               "' 无效: 期望 " +
                               detail::expected_type_zh(e.expected_type);
                    else if constexpr (std::same_as<
                                           T, pjh::cli::AmbiguousCommandError>)
                        return "命令 '" + e.input + "' 有歧义，候选: " +
                               detail::join_items(e.candidates, "、");
                    else if constexpr (std::same_as<T, pjh::cli::UnknownCommandError>)
                    {
                        if (e.suggestions.empty())
                            return "未知命令: '" + e.input + "'";
                        return "未知命令: '" + e.input + "'；您是否要找: " +
                               detail::join_items(e.suggestions, "、");
                    }
                    else if constexpr (std::same_as<
                                           T, pjh::cli::ValueOutOfRangeError>)
                        return "选项 '" + e.option_display + "' 的值 '" + e.raw_value +
                               "' 超出范围 [" + e.min + ", " + e.max + "]";
                    else if constexpr (std::same_as<T, pjh::cli::EnumValueError>)
                        return "选项 '" + e.option_display + "' 的值 '" + e.raw_value +
                               "' 无效: 期望以下之一: " +
                               detail::join_items(e.valid_choices, "、");
                    else if constexpr (std::same_as<
                                           T, pjh::cli::CommandDisabledError>)
                        return "命令 '" + e.command_name + "' 当前不可用";
                    else if constexpr (std::same_as<
                                           T, pjh::cli::ConflictingOptionsError>)
                        return "选项冲突: " +
                               detail::join_items(e.option_names, "、") +
                               " 不能同时使用";
                    else if constexpr (std::same_as<
                                           T, pjh::cli::RequiredOptionGroupError>)
                        return std::string(e.exactly_one ? "必须提供" : "至少提供") +
                               " " + detail::join_items(e.option_names, "、") +
                               " 之一";
                    else if constexpr (std::same_as<
                                           T, pjh::cli::OptionDoesNotAcceptValueError>)
                        return "选项 '" + e.option_display + "' 不接受值";
                    else if constexpr (std::same_as<
                                           T, pjh::cli::NoCommandMatchedError>)
                        return "没有匹配的命令";
                    else
                        return pjh::cli::format_error(info);  // 上游新增 variant 的英文兜底
                },
                info);
        }

        /**
         * @brief 把 CliError 渲染为用户可见文本。
         * @param err 框架错误（含类别与结构化负载）。
         * @return 运行时错误返回 what()（项目消息本已中文、无前缀）；解析错误返回
         *         中文前缀 + render_parse_error_zh(info)。
         * @note 不改 ErrorKind 语义：调用方仍按 kind()/退出码契约分派（解析 2、执行 1）。
         */
        inline std::string render_error_zh(const pjh::cli::CliError &err)
        {
            if (err.kind() == pjh::cli::ErrorKind::Runtime)
                return err.what();
            return std::string(kParseErrorPrefix) + render_parse_error_zh(err.info());
        }
    }  // namespace cli
}  // namespace tkw

#endif  // INCLUDE_TKW_CLI_ERROR_ZH_HPP
