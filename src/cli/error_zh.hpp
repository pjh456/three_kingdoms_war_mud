/**
 * @file   error_zh.hpp
 * @brief  CLI 错误中文渲染。
 * @details 框架 `CliError`、资源加载、存档读写与对局失败均渲染为面向用户的中文
 *          文本；批量入口与 REPL 共用同一渲染，保证两条路径文案一致。资源加载、
 *          存档读写与对局失败的根因标签在此统一拼装，CLI 命令体只负责取
 *          `Result` 错误值。
 * @note   只渲染不改退出码：解析错误（`Parse`）加中文前缀 + 中文正文，运行时错误
 *         （`Runtime`）原样返回 `what()`（项目自身消息本已中文、无前缀）。
 * @ingroup tkw_cli
 */
#ifndef INCLUDE_TKW_CLI_ERROR_ZH_HPP
#define INCLUDE_TKW_CLI_ERROR_ZH_HPP

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <pjh_cli.hpp>

#include "config/error.hpp"
#include "game/flow/factory.hpp"
#include "game/flow/loop.hpp"
#include "io/error.hpp"
#include "save/error.hpp"

namespace tkw
{
    namespace cli
    {
        /** @brief 解析错误统一前缀（替代框架英文 "Parse Error: "）。 */
        inline constexpr std::string_view kParseErrorPrefix = "参数错误: ";

        namespace detail
        {
            /**
             * @brief  把字符串列表按分隔符拼接。
             * @param[in] items 待拼接的字符串列表。
             * @param[in] sep   分隔符。
             * @return 空列表返回空串，否则为各项以 `sep` 连接的结果。
             */
            std::string join_items(
                const std::vector<std::string> &items, std::string_view sep);

            /**
             * @brief 框架类型名 → 中文；未命中回落原文。
             * @param[in] expected_type 框架 `TypeConversionError` 的 `expected_type`。
             * @return "integer"→"整数"、"float"→"浮点数"、"bool" 开头→"布尔值"，
             *         其余原样，避免半英半中。
             */
            std::string expected_type_zh(std::string_view expected_type);
        }  // namespace detail

        /**
         * @brief 把 CliError 的解析负载渲染为中文正文（不含前缀）。
         * @param[in] err 框架错误；按 append-only 的 ErrorTag 分派到对应负载字段。
         * @return 已知标签的中文文案逐字不变；未识别标签（上游新增）回落中文兜底，
         *         不再泄漏框架英文正文。
         * @note 分派基于 CliError::tag()：ErrorTag 与 ErrorInfo 顺序对应且
         *       append-only（不重编号、不复用），故新标签不会改变既有映射。switch
         *       不设 default，上游新增 ErrorTag 时 -Wswitch 会提醒补分支；未补时
         *       末尾中文兜底保证用户面不出现英文。
         */
        std::string render_parse_error_zh(const pjh::cli::CliError &err);

        /**
         * @brief 把 CliError 渲染为用户可见文本。
         * @param[in] err 框架错误（含类别与结构化负载）。
         * @return 运行时错误返回 what()（项目消息本已中文、无前缀）；解析错误返回
         *         中文前缀 + render_parse_error_zh(err)。
         * @note 不改 ErrorKind 语义：调用方仍按 kind()/退出码契约分派（解析 2、执行 1）。
         */
        std::string render_error_zh(const pjh::cli::CliError &err);

        /**
         * @brief 存档加载失败类别 → 中文根因标签。
         * @param[in] kind 存档读取器返回的失败类别。
         * @return 五个枚举值各自的中文标签；未命中回落「未知错误」。
         * @note 穷举 `SaveErrorKind`，新增枚举值时编译器以 `-Wswitch` 提示补充。
         */
        std::string_view save_error_kind_zh(save::SaveErrorKind kind);

        /**
         * @brief 文件 I/O 错误 → 中文根因标签。
         * @param[in] e 文件层收敛后的错误类别。
         * @param[in] writing 是否为写入路径：`NotExist` 在写侧表示父目录不存在，
         *        在读侧表示文件不存在，必须由调用方区分。
         * @return 各枚举值的中文标签；未命中回落「未知错误」。
         */
        std::string_view io_error_zh(io::IoError e, bool writing);

        /**
         * @brief 存档解析失败文案：`存档加载失败（<标签>）: <detail>`。
         * @param[in] e 存档读取器错误（`detail` 为字段路径或定位串）。
         * @return 中文标签 + 原始定位，保留根因与精确字段。
         */
        std::string render_save_error_zh(const save::SaveError &e);

        /**
         * @brief 读档失败文案：`读取存档失败（<标签>）: <path>`。
         * @param[in] file 被读取的存档路径。
         * @param[in] e 文件层错误；`NotExist` 按读侧语义渲染为「文件不存在」。
         * @return `读取存档失败（<标签>）: <path>` 的中文文案。
         */
        std::string render_read_error_zh(
            const std::filesystem::path &file, io::IoError e);

        /**
         * @brief 写档失败文案：`写入存档失败（<标签>）: <path>`。
         * @param[in] file 被写入的存档路径。
         * @param[in] e 文件层错误；`NotExist` 按写侧语义渲染为「父目录不存在」。
         * @return `写入存档失败（<标签>）: <path>` 的中文文案。
         */
        std::string render_write_error_zh(
            const std::filesystem::path &file, io::IoError e);

        /**
         * @brief 资源加载失败类别 → 中文根因标签。
         * @param[in] kind 配置层返回的失败类别。
         * @return 六个枚举值各自的中文标签；未命中回落「未知错误」。
         * @note 穷举 `ConfigErrorKind`，新增枚举值时编译器以 `-Wswitch` 提示补充。
         *       `detail` 为文件路径或字段路径，由调用方保留在标签之后。
         */
        std::string_view config_error_kind_zh(config::ConfigErrorKind kind);

        /**
         * @brief 对局流程错误 → 中文根因标签。
         * @param[in] code 非平局的流程错误。
         * @return 三个枚举值各自的中文标签；未命中回落「未知错误」。
         * @note 穷举 `LoopError`，新增枚举值时编译器以 `-Wswitch` 提示补充。
         *       `MaxRounds` 由调用方映射为平局，标签仅供异常直落路径使用。
         */
        std::string_view loop_error_label_zh(game::LoopError code);

        /**
         * @brief 回合根因 → 中文标签。
         * @param[in] e 回合流程返回的失败类别。
         * @return 十一个枚举值各自的中文标签；未命中回落「未知错误」。
         * @note 穷举 `TurnError`，新增枚举值时编译器以 `-Wswitch` 提示补充。
         *       仅用于 step/run 经根因出参透出的失败，一次性跑局不消费。
         */
        std::string_view turn_error_label_zh(game::TurnError e);

        /**
         * @brief 对局失败文案：`对局失败（<标签>）`。
         * @param[in] code 非 MaxRounds 的流程错误；达回合上限由调用方映射为平局。
         * @return 固定前缀 + 中文根因标签；对局错误无 detail 字段。
         * @note 仅一次性跑局与批量模拟使用；step/run 的运行期错误经
         *       `step_session` 根因出参单独渲染，不走本函数。
         */
        std::string loop_error_zh(game::LoopError code);

        namespace detail
        {
            /**
             * @brief 牌堆加载失败 → 用户可见文案（中文根因标签 + detail）。
             * @param[in] e 目录加载错误；detail 为文件路径或字段路径。
             * @return 固定前缀「加载牌堆失败」+ 类别中文标签 + detail 的文案。
             */
            std::string format_load_error(const config::ConfigError &e);

            /**
             * @brief 对局失败 → 带中文根因标签的用户可见文案。
             * @param[in] code 非 MaxRounds 的流程错误；达回合上限由调用方映射为平局。
             * @return 「对局失败（<标签>）」文案。
             * @note 仅一次性跑局与批量模拟使用；step/run 经根因出参走
             *       format_turn_failure，不经过本函数。
             */
            std::string format_loop_error(game::LoopError code);

            /**
             * @brief 回合失败 → 带中文根因标签的用户可见文案。
             * @param[in] code  step/run 循环返回的错误类别。
             * @param[in] root  根因出参写回的回合错误；仅 `TurnFailed` 有效。
             * @param[in] actor 失败回合的角色 id（调用方在推进前捕获）。
             * @return 「回合执行失败（角色 <actor>，<标签>）」；`NoPlayers`
             *         表示会话角色已不存在，标签回落「角色不存在」。
             * @note 仅 `TurnFailed` 消费 `root`；`MaxRounds` 由调用方映射为平局，
             *       不进入本函数。`TurnFailed` 追加恢复引导：失败回合已部分结算，
             *       但引擎已消费该回合并推进，可继续 step/run。
             */
            std::string format_turn_failure(
                game::LoopError code, game::TurnError root,
                const std::string &actor);

            /**
             * @brief  建局错误 → 用户可见文案。
             * @details 覆盖目录加载、玩家创建、身份局人数与未知武将四类错误面。
             * @param[in] e 建局错误。
             * @return 各类错误对应的中文文案；目录加载失败复用 `format_load_error`。
             */
            std::string format_build_error(const game::BuildError &e);
        }  // namespace detail
    }  // namespace cli
}  // namespace tkw

#endif  // INCLUDE_TKW_CLI_ERROR_ZH_HPP
