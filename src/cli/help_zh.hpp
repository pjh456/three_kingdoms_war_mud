/**
 * @file help_zh.hpp
 * @brief CLI 中文帮助渲染：把框架帮助/查询结构渲染为中文，供批量 --help 与 REPL 共用。
 * @note 只替换框架渲染结果的段标题与用法前缀，选项/参数/子命令的排布与对齐仍由框架
 *       负责；面向用户的字符串一律中文，批量帮助与 REPL 帮助走同一渲染。
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
            /** 把 "Usage: " 前缀换成中文，其余原样（帮助与 REPL 无匹配提示共用）。 */
            inline std::string zh_usage_prefix(std::string text)
            {
                constexpr std::string_view prefix = "Usage: ";
                if (text.starts_with(prefix))
                    text.replace(0, prefix.size(), "用法: ");
                return text;
            }

            /** 把帮助正文中独占一行的英文段标题替换为中文（只替换首个匹配）。 */
            inline void replace_heading_line(
                std::string &text, std::string_view from, std::string_view to)
            {
                const std::string needle = "\n" + std::string(from) + ":\n";
                const std::string replacement = "\n" + std::string(to) + ":\n";
                if (std::size_t pos = text.find(needle); pos != std::string::npos)
                    text.replace(pos, needle.size(), replacement);
            }

            /**
             * @brief 按命令名列表渲染「命令名（含别名）+ 描述」两列。
             * @param root  根命令，用于按名查子命令的别名与描述。
             * @param names 命令名列表（查询结果为规范名，别名从命令树取）。
             * @return 两列文本；无别名命令按命令名单列渲染，描述缺失留空。
             * @note 列宽按带别名后缀的名字计算，保证行对齐。
             */
            inline std::string command_lines_zh(
                const pjh::cli::BranchCommand &root,
                const std::vector<std::string> &names)
            {
                std::vector<std::string> lefts;
                std::vector<std::string> descs;
                lefts.reserve(names.size());
                descs.reserve(names.size());

                for (const auto &n : names)
                {
                    std::string left = n;
                    std::string desc;
                    const pjh::cli::BaseCommand *sub = root.find_subcommand(n);
                    if (sub != nullptr)
                    {
                        desc = sub->description();
                        const auto &aliases = sub->aliases();
                        if (!aliases.empty())
                        {
                            left += " (";
                            for (std::size_t i = 0; i < aliases.size(); ++i)
                            {
                                if (i > 0)
                                    left += ", ";
                                left += aliases[i];
                            }
                            left += ")";
                        }
                    }
                    lefts.push_back(std::move(left));
                    descs.push_back(std::move(desc));
                }

                std::size_t width = 0;
                for (const auto &l : lefts)
                    if (l.size() > width)
                        width = l.size();

                std::string out;
                for (std::size_t i = 0; i < names.size(); ++i)
                {
                    out += "  " + lefts[i];
                    out.append(width - lefts[i].size(), ' ');
                    out += "  ";
                    out += descs[i];
                    out += "\n";
                }
                return out;
            }
        }  // namespace detail

        /**
         * @brief 把命令树的框架帮助数据渲染为中文帮助。
         * @param cmd 请求帮助的命令（根或任一子命令）。
         * @return 中文段标题（用法/选项/公共选项/参数/子命令）的完整帮助；根命令额外附用法示例。
         * @note 只替换框架渲染结果的段标题与 usage 前缀，选项/参数/子命令的排布与
         *       对齐仍由框架负责，避免自造排版。段标题在渲染后再替换，使框架仍按
         *       英文段名选择列宽上限（选项段 32 字节）。选项标注（如 (repeatable)）
         *       保持框架原文。program_name 用完整命令路径，子命令帮助也带 tkw 前缀。
         *       根帮助示例按「批量一次性」与「REPL 会话」分组：会话流命令只能在
         *       `tkw repl` 内逐条输入，不带 tkw 前缀。
         */
        inline std::string render_help_zh(const pjh::cli::BaseCommand &cmd)
        {
            std::vector<std::string_view> parts;
            for (const pjh::cli::BaseCommand *c = &cmd; c != nullptr; c = c->parent())
                if (!c->name().empty())
                    parts.push_back(c->name());
            std::string path;
            for (auto it = parts.rbegin(); it != parts.rend(); ++it)
            {
                if (!path.empty())
                    path += ' ';
                path += *it;
            }

            pjh::cli::HelpInfo info = pjh::cli::HelpFormatter::collect_help(cmd, path);
            pjh::cli::HelpDocument doc = pjh::cli::HelpFormatter::build_document(info);

            std::string text =
                detail::zh_usage_prefix(pjh::cli::HelpFormatter::format_help(doc));
            detail::replace_heading_line(text, "Options", "选项");
            detail::replace_heading_line(text, "Inherited Options", "公共选项");
            detail::replace_heading_line(text, "Arguments", "参数");
            detail::replace_heading_line(text, "Subcommands", "子命令");

            if (cmd.parent() == nullptr)
                text +=
                    "示例:\n"
                    "  批量一次性:\n"
                    "    tkw                      跑一局 AI 对局\n"
                    "    tkw deal 2 1             按位置参数跑一局（2 人，种子 1）\n"
                    "    tkw --ai aggressive deal 2 1  aggressive AI 跑一局\n"
                    "    tkw audit                审计牌堆\n"
                    "    tkw cards              列出牌表构成\n"
                    "    tkw simulate 100 2     批量模拟 100 局（2 人）\n"
                    "  REPL 会话（先 tkw repl，再逐条输入）:\n"
                    "    new --players 2 --seed 1 开新局\n"
                    "    step                     执行一个回合\n"
                    "    status                   查看会话状态\n"
                    "    save s.json              保存当前对局\n"
                    "    load s.json              载入存档到会话，再 step 继续\n"
                    "  真人参与（先 tkw --human P0 repl，再逐条输入）:\n"
                    "    new --players 2 --seed 1 开新局\n"
                    "    step                     轮到 P0 时按提示输入（play/pass/discard）\n";
            return text;
        }

        /**
         * @brief REPL `?` 查询结果的中文渲染。
         * @param root   根命令，用于按名查子命令描述。
         * @param result 框架查询结果（列表/匹配/模糊/无匹配）。
         * @return 中文提示 + 命令名与描述两列；无匹配时附中文用法行。
         * @note 只消费框架结构，不重复实现匹配逻辑；描述直接取命令树，保证与
         *       --help 子命令表同源。
         */
        inline std::string render_query_zh(
            const pjh::cli::BranchCommand &root,
            const pjh::cli::QueryResult &result)
        {
            using pjh::cli::QueryKind;
            switch (result.kind)
            {
            case QueryKind::Listing:
                return "命令（? <关键词> 过滤，help <命令> 看用法）:\n" +
                       detail::command_lines_zh(root, result.names);
            case QueryKind::Matched:
                return "匹配命令:\n" + detail::command_lines_zh(root, result.names);
            case QueryKind::Fuzzy:
            {
                std::string out = "您是否要找:";
                for (const auto &m : result.suggestions.matches)
                    out += " " + m.name;
                return out + "\n";
            }
            case QueryKind::NoMatch:
                // 调用方统一在结果末尾补一个换行，这里不自带换行以免多出空行。
                return "没有匹配的命令。" + detail::zh_usage_prefix(result.usage_line);
            }
            return {};
        }

        /**
         * @brief REPL `help [命令]` 的中文渲染。
         * @param result 框架导航结果。
         * @return 根/子命令帮助走同一中文渲染；叶命令与未知命令用中文提示。
         * @note 帮助正文复用 render_help_zh，保证 REPL `help` 与批量 `--help` 同格式。
         */
        inline std::string render_help_nav_zh(
            const pjh::cli::HelpNavigationResult &result)
        {
            using pjh::cli::HelpNavigationKind;
            switch (result.kind)
            {
            case HelpNavigationKind::RootHelp:
            case HelpNavigationKind::SubcommandHelp:
                return render_help_zh(*result.resolved);
            case HelpNavigationKind::NonBranch:
                return "'" + result.failed_command_name + "' 没有子命令。\n";
            case HelpNavigationKind::UnknownCommand:
            {
                std::string out = "未知命令 '" + result.failed_token + "'。";
                if (!result.suggestions.matches.empty())
                {
                    out += " 您是否要找:";
                    for (const auto &m : result.suggestions.matches)
                        out += " " + m.name;
                }
                return out + "\n";
            }
            }
            return {};
        }
    }  // namespace cli
}  // namespace tkw

#endif  // INCLUDE_TKW_CLI_HELP_ZH_HPP
