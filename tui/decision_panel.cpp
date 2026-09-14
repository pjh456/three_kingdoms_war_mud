/**
 * @file decision_panel.cpp
 * @brief 决策面板实现：键位状态机与 FTXUI 渲染。
 * @note 只读纯值视图，候选文本已在 worker 折好；本文件不访问目录/引擎，也不写
 *       标准输出，避免污染全屏画面。
 */

#include "decision_panel.hpp"

#include <charconv>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <ftxui/ftxui.hpp>

namespace tkw
{
    namespace tui
    {
        namespace
        {
            /** 命令缓冲长度上限，防无界追加。 */
            constexpr std::size_t kComposeMax = 12;

            /** 数字键 '1'..'9' → 0 基候选下标；非数字返回 false。 */
            bool digit_index(const ftxui::Event &event, std::size_t &out)
            {
                if (!event.is_character())
                    return false;
                const std::string ch = event.character();
                if (ch.size() != 1 || ch[0] < '1' || ch[0] > '9')
                    return false;
                out = static_cast<std::size_t>(ch[0] - '1');
                return true;
            }

            /**
             * @brief 解析 `card <序号>` 命令缓冲中的 1 基序号。
             * @param compose 命令缓冲；接受 c<n> / c <n> / card<n> / card <n>，
             *                空白忽略、大小写不敏感。
             * @param count   候选数量；序号合法区间为 [1, count]。
             * @param out     出参：合法时写入 0 基候选下标。
             * @return 合法返回 true；缺序号、非数字或越界返回 false。
             */
            bool parse_detail_index(
                const std::string &compose, std::size_t count, int &out)
            {
                std::string token;
                for (const char ch : compose)
                    if (ch != ' ' && ch != '\t')
                        token.push_back(ch);
                if (token.empty())
                    return false;

                std::size_t pos = 0;
                if (token[pos] == 'c' || token[pos] == 'C')
                    ++pos;
                if (pos < token.size() && (token[pos] == 'a' || token[pos] == 'A'))
                {
                    if (pos + 3 > token.size() || (token[pos + 1] != 'r' &&
                                                   token[pos + 1] != 'R') ||
                        (token[pos + 2] != 'd' && token[pos + 2] != 'D'))
                        return false;
                    pos += 3;
                }
                if (pos >= token.size())
                    return false;

                int value = 0;
                const char *first = token.data() + pos;
                const char *last = token.data() + token.size();
                const auto result = std::from_chars(first, last, value);
                if (result.ec != std::errc() || result.ptr != last)
                    return false;
                if (value < 1 || value > static_cast<int>(count))
                    return false;
                out = value - 1;
                return true;
            }
        }  // namespace

        void DecisionPanel::show(DecisionPanelView view)
        {
            m_cursor = 0;
            m_checked.assign(view.options.size(), false);
            m_notice.clear();
            m_compose.clear();
            m_detail_index = -1;
            m_show_help = false;
            m_view = std::move(view);
        }

        void DecisionPanel::hide()
        {
            m_view.reset();
            m_checked.clear();
            m_cursor = 0;
            m_notice.clear();
            m_compose.clear();
            m_detail_index = -1;
            m_show_help = false;
        }

        void DecisionPanel::move_cursor(int delta)
        {
            if (!m_view || m_view->options.empty())
                return;
            const int count = static_cast<int>(m_view->options.size());
            int next = static_cast<int>(m_cursor) + delta;
            if (next < 0)
                next = 0;
            if (next >= count)
                next = count - 1;
            m_cursor = static_cast<std::size_t>(next);
            m_detail_index = -1;
        }

        void DecisionPanel::toggle_cursor()
        {
            if (!m_view || !m_view->multi || m_cursor >= m_checked.size())
                return;
            m_checked[m_cursor] = !m_checked[m_cursor];
            m_notice.clear();
        }

        std::vector<std::size_t> DecisionPanel::selected() const
        {
            std::vector<std::size_t> out;
            if (!m_view)
                return out;
            if (m_view->multi)
            {
                for (std::size_t i = 0; i < m_checked.size(); ++i)
                    if (m_checked[i])
                        out.push_back(i);
                return out;
            }
            if (!m_view->options.empty())
                out.push_back(m_cursor);
            return out;
        }

        void DecisionPanel::confirm()
        {
            if (!m_view)
                return;

            if (m_view->options.empty())
            {
                if (!m_view->allow_pass)
                {
                    m_notice = "没有可选项，无法继续";
                    return;
                }
                if (m_on_submit && m_on_submit({}, true))
                    hide();
                else
                    m_notice = "放弃被拒绝，请重试";
                return;
            }

            std::vector<std::size_t> selection = selected();
            if (m_view->multi &&
                static_cast<int>(selection.size()) != m_view->need_count)
            {
                const int missing =
                    m_view->need_count - static_cast<int>(selection.size());
                m_notice = "还需选择 " + std::to_string(missing) + " 张（当前 " +
                          std::to_string(selection.size()) + "/" +
                          std::to_string(m_view->need_count) + "）";
                return;
            }
            if (m_on_submit && m_on_submit(std::move(selection), false))
                hide();
            else
                m_notice = "提交被拒绝，请重试";
        }

        void DecisionPanel::discard()
        {
            if (!m_view)
                return;
            if (!m_view->allow_pass)
            {
                m_notice = "本决策不能放弃";
                return;
            }
            if (m_on_submit && m_on_submit({}, true))
                hide();
            else
                m_notice = "放弃被拒绝，请重试";
        }

        bool DecisionPanel::handle_compose(const ftxui::Event &event)
        {
            if (event == ftxui::Event::Return)
            {
                int index = -1;
                if (parse_detail_index(m_compose, m_view->options.size(), index))
                {
                    const PanelOption &opt =
                        m_view->options[static_cast<std::size_t>(index)];
                    if (!opt.hidden && opt.card_name.empty() &&
                        opt.card_meta.empty() && opt.card_text.empty())
                    {
                        m_detail_index = -1;
                        m_notice = "该候选项没有牌面";
                    }
                    else
                    {
                        m_detail_index = index;
                        m_show_help = false;
                        m_notice.clear();
                    }
                }
                else
                {
                    m_detail_index = -1;
                    m_notice = "用法：card <序号>（或 c <序号>）";
                }
                m_compose.clear();
                return true;
            }

            if (event == ftxui::Event::Backspace)
            {
                if (!m_compose.empty())
                    m_compose.pop_back();
                if (m_compose.empty())
                    m_notice.clear();  // 退出 card 输入态：不留「输入序号…」提示
                return true;
            }

            if (event.is_character())
            {
                const std::string ch = event.character();
                if (m_compose.size() + ch.size() <= kComposeMax)
                    m_compose += ch;
                return true;
            }

            // 其余按键在命令输入期间吞掉，避免误改光标或提交。
            return true;
        }

        bool DecisionPanel::on_event(const ftxui::Event &event)
        {
            if (!m_view)
                return false;

            // 命令缓冲优先：card 输入期间数字只入缓冲，不落入直选/提交。
            if (!m_compose.empty())
                return handle_compose(event);

            if (event == ftxui::Event::Character('?'))
            {
                m_show_help = !m_show_help;
                m_detail_index = -1;
                m_notice.clear();
                return true;
            }
            if (event == ftxui::Event::c || event == ftxui::Event::C)
            {
                m_compose = "c";
                m_detail_index = -1;
                m_notice = "输入序号后回车看牌面（card <序号>）";
                return true;
            }

            if (event == ftxui::Event::ArrowUp)
            {
                move_cursor(-1);
                return true;
            }
            if (event == ftxui::Event::ArrowDown)
            {
                move_cursor(1);
                return true;
            }
            if (event == ftxui::Event::Return)
            {
                confirm();
                return true;
            }
            if (m_view->multi && event == ftxui::Event::Character(' '))
            {
                toggle_cursor();
                return true;
            }
            if (event == ftxui::Event::p || event == ftxui::Event::P)
            {
                discard();
                return true;
            }

            std::size_t index = 0;
            if (digit_index(event, index))
            {
                if (index >= m_view->options.size())
                    return true;  // 越界数字吞掉，避免落入隐藏的命令输入
                m_cursor = index;
                if (m_view->multi)
                    toggle_cursor();
                else
                    confirm();
                return true;
            }
            return false;
        }

        ftxui::Element DecisionPanel::render() const
        {
            if (!m_view)
                return ftxui::text("");

            std::vector<ftxui::Element> rows;

            if (m_view->multi)
            {
                std::size_t count = 0;
                for (const bool picked : m_checked)
                    count += picked ? 1 : 0;
                rows.push_back(ftxui::text("已选 " + std::to_string(count) + "/" +
                                           std::to_string(m_view->need_count)) |
                               ftxui::dim);
            }

            if (m_view->options.empty())
                rows.push_back(ftxui::text("（无候选）"));

            for (std::size_t i = 0; i < m_view->options.size(); ++i)
            {
                std::string line;
                if (m_view->multi)
                    line = (i < m_checked.size() && m_checked[i]) ? "[x] " : "[ ] ";
                else
                    line = (i == m_cursor) ? "> " : "  ";
                line += std::to_string(i + 1) + ". " + m_view->options[i].text;

                ftxui::Element row = ftxui::text(line);
                if (!m_view->multi && i == m_cursor)
                    row = ftxui::inverted(std::move(row));
                rows.push_back(std::move(row));
            }

            rows.push_back(ftxui::separatorLight());

            if (m_show_help)
            {
                rows.push_back(
                    ftxui::text(
                        "键位：↑/↓ 选择  Enter 确认  p 放弃  空格多选  数字直选") |
                    ftxui::dim);
                rows.push_back(
                    ftxui::text("card <序号> / c <序号>：查看候选牌面（回车生效）") |
                    ftxui::dim);
                rows.push_back(ftxui::text("? 显示/隐藏本说明  q/Esc 退出") |
                               ftxui::dim);
            }
            else if (m_detail_index >= 0 &&
                     static_cast<std::size_t>(m_detail_index) <
                         m_view->options.size())
            {
                const PanelOption &opt =
                    m_view->options[static_cast<std::size_t>(m_detail_index)];
                std::string face =
                    "牌面：" + (opt.card_name.empty() ? opt.text : opt.card_name);
                if (!opt.card_meta.empty())
                    face += "  " + opt.card_meta;
                if (!opt.second_card_name.empty())
                    face += "  +  " + opt.second_card_name;
                if (!opt.second_card_meta.empty())
                    face += "  " + opt.second_card_meta;
                rows.push_back(ftxui::text(face) | ftxui::dim);

                std::string desc;
                if (opt.hidden)
                    desc = "（未知手牌，无法查看）";
                else
                {
                    if (!opt.card_text.empty())
                        desc = opt.card_text;
                    if (!opt.second_card_text.empty())
                    {
                        if (!desc.empty())
                            desc += "  /  ";
                        desc += opt.second_card_text;
                    }
                    if (desc.empty())
                        desc = "（无说明）";
                }
                rows.push_back(ftxui::paragraph("说明：" + desc) | ftxui::dim |
                               ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, 3));
            }

            std::string hint = "↑/↓ 选择  Enter 确认";
            if (m_view->allow_pass)
                hint += "  p 放弃";
            if (m_view->multi)
                hint += "  空格多选";
            hint += "  数字直选  c 看牌面  ? 帮助  q/Esc 退出";
            rows.push_back(ftxui::text(hint) | ftxui::dim);

            if (!m_notice.empty())
                rows.push_back(ftxui::text(m_notice) |
                               ftxui::color(ftxui::Color::Red));

            const std::string title = m_view->actor.empty()
                                          ? m_view->title
                                          : m_view->actor + "：" + m_view->title;
            return ftxui::window(ftxui::text(title) | ftxui::bold,
                                 ftxui::vbox(std::move(rows)));
        }
    }  // namespace tui
}  // namespace tkw
