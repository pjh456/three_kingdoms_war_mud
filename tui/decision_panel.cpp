/**
 * @file decision_panel.cpp
 * @brief 决策面板实现：键位状态机与 FTXUI 渲染。
 * @note 只读纯值视图，候选文本已在 worker 折好；本文件不访问目录/引擎，也不写
 *       标准输出，避免污染全屏画面。
 */

#include "decision_panel.hpp"

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
        }  // namespace

        void DecisionPanel::show(DecisionPanelView view)
        {
            cursor_ = 0;
            checked_.assign(view.options.size(), false);
            notice_.clear();
            view_ = std::move(view);
        }

        void DecisionPanel::hide()
        {
            view_.reset();
            checked_.clear();
            cursor_ = 0;
            notice_.clear();
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

        bool DecisionPanel::on_event(const ftxui::Event &event)
        {
            if (!view_)
                return false;

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

            std::string hint = "↑/↓ 选择  Enter 确认";
            if (view_->allow_pass)
                hint += "  p 放弃";
            if (view_->multi)
                hint += "  空格多选";
            hint += "  数字直选  q/Esc 退出";
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
