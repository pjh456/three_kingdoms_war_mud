/**
 * @file   help_zh.hpp
 * @brief  CLI 中文帮助渲染。
 * @details 把框架帮助/查询结构渲染为中文，供批量 `--help` 与 REPL 共用；只替换
 *          框架渲染结果的段标题与用法前缀，选项/参数/子命令的排布与对齐仍由框架
 *          负责，面向用户的字符串一律中文。
 * @ingroup tkw_cli
 */
#ifndef INCLUDE_TKW_CLI_HELP_ZH_HPP
#define INCLUDE_TKW_CLI_HELP_ZH_HPP

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <pjh_cli.hpp>
#include <pjh_cli/console/help_navigator.hpp>
#include <pjh_cli/console/query_result.hpp>

namespace tkw
{
    namespace cli
    {
        namespace detail
        {
            /**
             * @brief  把 "Usage: " 前缀换成中文，其余原样。
             * @param[in] text 框架渲染的用法文本。
             * @return 前缀已中文化的文本；无该前缀时原样返回。
             * @note   帮助与 REPL 无匹配提示共用。
             */
            std::string zh_usage_prefix(std::string text);

            /**
             * @brief  把帮助正文中独占一行的英文段标题替换为中文。
             * @param[in,out] text 帮助正文；命中时原地替换。
             * @param[in]     from 英文段标题。
             * @param[in]     to   中文段标题。
             * @note   只替换首个匹配。
             */
            void replace_heading_line(
                std::string &text, std::string_view from, std::string_view to);

            /**
             * @brief 按命令名列表渲染「命令名（含别名）+ 描述」两列。
             * @param[in] root  根命令，用于按名查子命令的别名与描述。
             * @param[in] names 命令名列表（查询结果为规范名，别名从命令树取）。
             * @return 两列文本；无别名命令按命令名单列渲染，描述缺失留空。
             * @note 列宽按带别名后缀的名字计算，保证行对齐。
             */
            std::string command_lines_zh(
                const pjh::cli::BranchCommand &root,
                const std::vector<std::string> &names);

            /**
             * @brief 高频 leaf 命令的单命令用法示例（根帮助「示例」段的叶子版）。
             * @param[in] name 命令规范名（即 cmd.name()）。
             * @return 「示例:」段文本；无示例的命令返回空串。
             * @note 会话流命令在 REPL 内逐条输入，示例给命令名形态并附前置步骤；
             *       一次性命令给 `tkw <命令>` 形态。示例必须与当前选项面一致，
             *       增删选项或位置参数时同步复核本表。
             */
            std::string leaf_help_examples(std::string_view name);
        }  // namespace detail

        /**
         * @brief 把命令树的框架帮助数据渲染为中文帮助。
         * @param[in] cmd 请求帮助的命令（根或任一子命令）。
         * @return 中文段标题（用法/选项/公共选项/参数/子命令）的完整帮助；根命令附
         *         分组用法示例，高频叶子命令附单命令示例。
         * @note 只替换框架渲染结果的段标题与 usage 前缀，选项/参数/子命令的排布与
         *       对齐仍由框架负责，避免自造排版。段标题在渲染后再替换，使框架仍按
         *       英文段名选择列宽上限（选项段 32 字节）。选项标注（如 (repeatable)）
         *       保持框架原文。program_name 用完整命令路径，子命令帮助也带 tkw 前缀。
         *       根帮助示例按「第一局」「批量一次性」与「REPL 会话」分组：会话流命令只能
         *       在 `tkw repl` 内逐条输入，不带 tkw 前缀。叶子示例只覆盖高频命令，见
         *       detail::leaf_help_examples。
         */
        std::string render_help_zh(const pjh::cli::BaseCommand &cmd);

        /**
         * @brief REPL `?` 查询结果的中文渲染。
         * @param[in] root   根命令，用于按名查子命令描述。
         * @param[in] result 框架查询结果（列表/匹配/模糊/无匹配）。
         * @return 中文提示 + 命令名与描述两列；无匹配时附中文用法行。
         * @note 只消费框架结构，不重复实现匹配逻辑；描述直接取命令树，保证与
         *       --help 子命令表同源。
         */
        std::string render_query_zh(
            const pjh::cli::BranchCommand &root,
            const pjh::cli::QueryResult &result);

        /**
         * @brief REPL `help [命令]` 的中文渲染。
         * @param[in] result 框架导航结果。
         * @return 根/子命令帮助走同一中文渲染；叶命令与未知命令用中文提示。
         * @note 帮助正文复用 render_help_zh，保证 REPL `help` 与批量 `--help` 同格式。
         */
        std::string render_help_nav_zh(
            const pjh::cli::HelpNavigationResult &result);
    }  // namespace cli
}  // namespace tkw

#endif  // INCLUDE_TKW_CLI_HELP_ZH_HPP
