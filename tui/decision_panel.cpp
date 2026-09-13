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
            cursor_ = 0;
            checked_.assign(view.options.size(), false);
            notice_.clear();
            compose_.clear();
            detail_index_ = -1;
            show_help_ = false;
            view_ = std::move(view);
        }

        void DecisionPanel::hide()
        {
            view_.reset();
            checked_.clear();
            cursor_ = 0;
            notice_.clear();
            compose_.clear();
            detail_index_ = -1;
            show_help_ = false;
        }

        void DecisionPanel::move_cursor(int delta)
        {
            if (!view_ || view_->options.empty())
                return;
            const int count = static_cast<int>(view_->options.size());
            int next = static_cast<int>(cursor_) + delta;
            if (next < 0)
                next = 0;
            if (next >= count)
                next = count - 1;
            cursor_ = static_cast<std::size_t>(next);
            detail_index_ = -1;
        }

        void DecisionPanel::toggle_cursor()
        {
            if (!view_ || !view_->multi || cursor_ >= checked_.size())
                return;
            checked_[cursor_] = !checked_[cursor_];
            notice_.clear();
        }

        std::vector<std::size_t> DecisionPanel::selected() const
        {
            std::vector<std::size_t> out;
            if (!view_)
                return out;
            if (view_->multi)
            {
                for (std::size_t i = 0; i < checked_.size(); ++i)
                    if (checked_[i])
                        out.push_back(i);
                return out;
            }
            if (!view_->options.empty())
                out.push_back(cursor_);
            return out;
        }

        void DecisionPanel::confirm()
        {
            if (!view_)
                return;

            if (view_->options.empty())
            {
                if (!view_->allow_pass)
                {
                    notice_ = "没有可选项，无法继续";
                    return;
                }
                if (on_submit_ && on_submit_({}, true))
                    hide();
                else
                    notice_ = "放弃被拒绝，请重试";
                return;
            }

            std::vector<std::size_t> selection = selected();
            if (view_->multi &&
                static_cast<int>(selection.size()) != view_->need_count)
            {
                const int missing =
                    view_->need_count - static_cast<int>(selection.size());
                notice_ = "还需选择 " + std::to_string(missing) + " 张（当前 " +
                          std::to_string(selection.size()) + "/" +
                          std::to_string(view_->need_count) + "）";
                return;
            }
            if (on_submit_ && on_submit_(std::move(selection), false))
                hide();
            else
                notice_ = "提交被拒绝，请重试";
        }

        void DecisionPanel::discard()
        {
            if (!view_)
                return;
            if (!view_->allow_pass)
            {
                notice_ = "本决策不能放弃";
                return;
            }
            if (on_submit_ && on_submit_({}, true))
                hide();
            else
                notice_ = "放弃被拒绝，请重试";
        }

        bool DecisionPanel::handle_compose(const ftxui::Event &event)
        {
            if (event == ftxui::Event::Return)
            {
                int index = -1;
                if (parse_detail_index(compose_, view_->options.size(), index))
                {
                    const PanelOption &opt =
                        view_->options[static_cast<std::size_t>(index)];
                    if (!opt.hidden && opt.card_name.empty() &&
                        opt.card_meta.empty() && opt.card_text.empty())
                    {
                        detail_index_ = -1;
                        notice_ = "该候选项没有牌面";
                    }
                    else
                    {
                        detail_index_ = index;
                        show_help_ = false;
                        notice_.clear();
                    }
                }
                else
                {
                    detail_index_ = -1;
                    notice_ = "用法：card <序号>（或 c <序号>）";
                }
                compose_.clear();
                return true;
            }

            if (event == ftxui::Event::Backspace)
            {
                if (!compose_.empty())
                    compose_.pop_back();
                if (compose_.empty())
                    notice_.clear();  // 退出 card 输入态：不留「输入序号…」提示
                return true;
            }

            if (event.is_character())
            {
                const std::string ch = event.character();
                if (compose_.size() + ch.size() <= kComposeMax)
                    compose_ += ch;
                return true;
            }

            // 其余按键在命令输入期间吞掉，避免误改光标或提交。
            return true;
        }

        bool DecisionPanel::on_event(const ftxui::Event &event)
        {
            if (!view_)
                return false;

            // 命令缓冲优先：card 输入期间数字只入缓冲，不落入直选/提交。
            if (!compose_.empty())
                return handle_compose(event);

            if (event == ftxui::Event::Character('?'))
            {
                show_help_ = !show_help_;
                detail_index_ = -1;
                notice_.clear();
                return true;
            }
            if (event == ftxui::Event::c || event == ftxui::Event::C)
            {
                compose_ = "c";
                detail_index_ = -1;
                notice_ = "输入序号后回车看牌面（card <序号>）";
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
            if (view_->multi && event == ftxui::Event::Character(' '))
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
                if (index >= view_->options.size())
                    return true;  // 越界数字吞掉，避免落入隐藏的命令输入
                cursor_ = index;
                if (view_->multi)
                    toggle_cursor();
                else
                    confirm();
                return true;
            }
            return false;
        }

        ftxui::Element DecisionPanel::render() const
        {
            if (!view_)
                return ftxui::text("");

            std::vector<ftxui::Element> rows;

            if (view_->multi)
            {
                std::size_t count = 0;
                for (const bool picked : checked_)
                    count += picked ? 1 : 0;
                rows.push_back(ftxui::text("已选 " + std::to_string(count) + "/" +
                                           std::to_string(view_->need_count)) |
                               ftxui::dim);
            }

            if (view_->options.empty())
                rows.push_back(ftxui::text("（无候选）"));

            for (std::size_t i = 0; i < view_->options.size(); ++i)
            {
                std::string line;
                if (view_->multi)
                    line = (i < checked_.size() && checked_[i]) ? "[x] " : "[ ] ";
                else
                    line = (i == cursor_) ? "> " : "  ";
                line += std::to_string(i + 1) + ". " + view_->options[i].text;

                ftxui::Element row = ftxui::text(line);
                if (!view_->multi && i == cursor_)
                    row = ftxui::inverted(std::move(row));
                rows.push_back(std::move(row));
            }

            rows.push_back(ftxui::separatorLight());

            if (show_help_)
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
            else if (detail_index_ >= 0 &&
                     static_cast<std::size_t>(detail_index_) <
                         view_->options.size())
            {
                const PanelOption &opt =
                    view_->options[static_cast<std::size_t>(detail_index_)];
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
            if (view_->allow_pass)
                hint += "  p 放弃";
            if (view_->multi)
                hint += "  空格多选";
            hint += "  数字直选  c 看牌面  ? 帮助  q/Esc 退出";
            rows.push_back(ftxui::text(hint) | ftxui::dim);

            if (!notice_.empty())
                rows.push_back(ftxui::text(notice_) |
                               ftxui::color(ftxui::Color::Red));

            const std::string title = view_->actor.empty()
                                          ? view_->title
                                          : view_->actor + "：" + view_->title;
            return ftxui::window(ftxui::text(title) | ftxui::bold,
                                 ftxui::vbox(std::move(rows)));
        }
    }  // namespace tui
}  // namespace tkw
