/**
 * @file error_zh.hpp
 * @brief CLI 错误中文渲染：框架 CliError 与资源/存档/对局失败均渲染为面向用户的中文文本。
 * @note 只渲染不改退出码：解析错误（Parse）加中文前缀 + 中文正文，运行时错误
 *       （Runtime）原样返回 what()（项目自身消息本已中文、无前缀）。批量入口与
 *       REPL 共用同一渲染，保证两条路径文案一致。资源加载、存档读写与对局失败的
 *       根因标签在此统一拼装，CLI 命令体只负责取 Result 错误值。
 */
#ifndef INCLUDE_TKW_CLI_ERROR_ZH_HPP
#define INCLUDE_TKW_CLI_ERROR_ZH_HPP

#include <concepts>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include <pjh_cli.hpp>

#include "config/error.hpp"
#include "game/flow/loop.hpp"
#include "io/error.hpp"
#include "save/error.hpp"

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

        /**
         * @brief 存档加载失败类别 → 中文根因标签。
         * @param kind 存档读取器返回的失败类别。
         * @return 五个枚举值各自的中文标签；未命中回落「未知错误」。
         * @note 穷举 `SaveErrorKind`，新增枚举值时编译器以 `-Wswitch` 提示补充。
         */
        inline std::string_view save_error_kind_zh(save::SaveErrorKind kind)
        {
            switch (kind)
            {
            case save::SaveErrorKind::ParseError:
                return "JSON 非法";
            case save::SaveErrorKind::VersionMismatch:
                return "存档版本不符";
            case save::SaveErrorKind::DeckMismatch:
                return "牌表不符";
            case save::SaveErrorKind::StructureError:
                return "结构错误";
            case save::SaveErrorKind::RngError:
                return "随机源错误";
            }
            return "未知错误";
        }

        /**
         * @brief 文件 I/O 错误 → 中文根因标签。
         * @param e 文件层收敛后的错误类别。
         * @param writing 是否为写入路径：`NotExist` 在写侧表示父目录不存在，
         *        在读侧表示文件不存在，必须由调用方区分。
         * @return 各枚举值的中文标签；未命中回落「未知错误」。
         */
        inline std::string_view io_error_zh(io::IoError e, bool writing)
        {
            switch (e)
            {
            case io::IoError::NotExist:
                return writing ? "父目录不存在" : "文件不存在";
            case io::IoError::NotAFile:
                return "不是常规文件";
            case io::IoError::Permission:
                return "无访问权限";
            case io::IoError::IoFailed:
                return "读写失败";
            }
            return "未知错误";
        }

        /**
         * @brief 存档解析失败文案：`存档加载失败（<标签>）: <detail>`。
         * @param e 存档读取器错误（`detail` 为字段路径或定位串）。
         * @return 中文标签 + 原始定位，保留根因与精确字段。
         */
        inline std::string render_save_error_zh(const save::SaveError &e)
        {
            return "存档加载失败（" + std::string(save_error_kind_zh(e.kind)) +
                   "）: " + e.detail;
        }

        /**
         * @brief 读档失败文案：`读取存档失败（<标签>）: <path>`。
         * @param file 被读取的存档路径。
         * @param e 文件层错误；`NotExist` 按读侧语义渲染为「文件不存在」。
         */
        inline std::string render_read_error_zh(
            const std::filesystem::path &file, io::IoError e)
        {
            return "读取存档失败（" + std::string(io_error_zh(e, false)) +
                   "）: " + file.string();
        }

        /**
         * @brief 写档失败文案：`写入存档失败（<标签>）: <path>`。
         * @param file 被写入的存档路径。
         * @param e 文件层错误；`NotExist` 按写侧语义渲染为「父目录不存在」。
         */
        inline std::string render_write_error_zh(
            const std::filesystem::path &file, io::IoError e)
        {
            return "写入存档失败（" + std::string(io_error_zh(e, true)) +
                   "）: " + file.string();
        }

        /**
         * @brief 资源加载失败类别 → 中文根因标签。
         * @param kind 配置层返回的失败类别。
         * @return 六个枚举值各自的中文标签；未命中回落「未知错误」。
         * @note 穷举 `ConfigErrorKind`，新增枚举值时编译器以 `-Wswitch` 提示补充。
         *       `detail` 为文件路径或字段路径，由调用方保留在标签之后。
         */
        inline std::string_view config_error_kind_zh(config::ConfigErrorKind kind)
        {
            switch (kind)
            {
            case config::ConfigErrorKind::FileNotFound:
                return "文件不存在";
            case config::ConfigErrorKind::IoFailed:
                return "文件读写失败";
            case config::ConfigErrorKind::ParseError:
                return "JSON 非法";
            case config::ConfigErrorKind::MissingField:
                return "缺少字段";
            case config::ConfigErrorKind::TypeMismatch:
                return "字段类型不符";
            case config::ConfigErrorKind::InvalidValue:
                return "字段值非法";
            }
            return "未知错误";
        }

        /**
         * @brief 对局流程错误 → 中文根因标签。
         * @param code 非平局的流程错误。
         * @return 三个枚举值各自的中文标签；未命中回落「未知错误」。
         * @note 穷举 `LoopError`，新增枚举值时编译器以 `-Wswitch` 提示补充。
         *       `MaxRounds` 由调用方映射为平局，标签仅供异常直落路径使用。
         */
        inline std::string_view loop_error_label_zh(game::LoopError code)
        {
            switch (code)
            {
            case game::LoopError::NoPlayers:
                return "无可用玩家";
            case game::LoopError::TurnFailed:
                return "回合流程失败";
            case game::LoopError::MaxRounds:
                return "达到最大回合数";
            }
            return "未知错误";
        }

        /**
         * @brief 对局失败文案：`对局失败（<标签>）`。
         * @param code 非 MaxRounds 的流程错误；达回合上限由调用方映射为平局。
         * @return 固定前缀 + 中文根因标签；对局错误无 detail 字段。
         * @note 仅一次性跑局与批量模拟使用；step/run 的运行期错误不带 code。
         */
        inline std::string loop_error_zh(game::LoopError code)
        {
            return "对局失败（" + std::string(loop_error_label_zh(code)) + "）";
        }
    }  // namespace cli
}  // namespace tkw

#endif  // INCLUDE_TKW_CLI_ERROR_ZH_HPP
