/**
 * @file app.cpp
 * @brief TUI 壳实现：命令输入与真人决策面板接线，整屏 DOM 交给渲染层。
 * @note 渲染层不触引擎容器：快照与日志都是控制器回送的值拷贝，worker 运行
 *       期间主线程只读模型。屏幕回送经 screen.Post（FTXUI TaskQueue 自带锁），
 *       闭包只捕获模型 shared_ptr 与值，不捕获 App。
 */

#include <cstddef>
#include <string>
#include <utility>

#include <ftxui/ftxui.hpp>

#include "app.hpp"
#include "render.hpp"

namespace
{
    /** 终端尺寸不可用时的回退行列，保证布局分级有确定输入。 */
    constexpr int kFallbackCols = 80;
    constexpr int kFallbackRows = 24;
}  // namespace

namespace tkw
{
    namespace tui
    {
        void App::bootstrap()
        {
            notice_ = "new / deal / step / run(r) / status(st) / save(w) / "
                      "load(l) / cards / rules / audit / quit(q)；"
                      "help 或 ? 查看用法，Esc / Ctrl-C 退出";
            controller_.bootstrap();
        }

        void App::sync_decision()
        {
            if (decision_panel_.visible())
                return;
            DecisionPanelView view;
            if (controller_.fetch_new_decision(view))
                decision_panel_.show(std::move(view));
        }

        ftxui::Element App::render() const
        {
            ftxui::Element bottom;
            if (decision_panel_.visible())
                bottom = decision_panel_.render();
            else if (input_)
                bottom = ftxui::hbox({ftxui::text("> "), input_->Render()});
            else
                bottom = ftxui::text("");

            ftxui::Dimensions size = ftxui::Terminal::Size();
            if (size.dimx <= 0 || size.dimy <= 0)
                size = ftxui::Dimensions{kFallbackCols, kFallbackRows};

            const detail::ShellSpec spec{controller_.snapshot(),
                                         controller_.log_lines(),
                                         notice_,
                                         std::move(bottom),
                                         size,
                                         log_ratio(),
                                         decision_panel_.visible()};
            return detail::render_shell(spec);
        }

        float App::log_ratio() const
        {
            return log_scroll_.ratio(controller_.log_lines().size());
        }

        ftxui::Component App::component(ftxui::ScreenInteractive &screen)
        {
            // 回送经 screen.Post；TaskQueue 自带锁，worker 可安全入队。
            // PostEvent(Custom) 触发重绘，闭包只改控制器模型值。
            controller_.set_post(
                [&screen](std::function<void()> task)
                {
                    screen.Post(
                        [&screen, task = std::move(task)]() mutable
                        {
                            task();
                            screen.PostEvent(ftxui::Event::Custom);
                        });
                });
            controller_.set_on_quit([&screen] { screen.Exit(); });
            decision_panel_.set_on_submit(
                [this](std::vector<std::size_t> selected, bool pass)
                {
                    return controller_.submit_decision(std::move(selected),
                                                       pass);
                });

            ftxui::InputOption input_option;
            input_option.content = &command_input_;
            input_option.placeholder = "输入命令（help 或 ? 查看用法）";
            input_option.multiline = false;
            input_option.on_enter =
                [this]
                {
                    std::string line = command_input_;
                    command_input_.clear();
                    controller_.execute_line(line);
                };
            input_ = ftxui::Input(input_option);

            auto container = ftxui::Container::Vertical({input_});
            return ftxui::Renderer(container, [this] { return render(); }) |
                   ftxui::CatchEvent(
                       [this](ftxui::Event event)
                       {
                           if (event == ftxui::Event::Escape ||
                               event == ftxui::Event::CtrlC)
                           {
                               controller_.request_quit();
                               return true;
                           }

                           // 面板唤醒与渲染同帧：先取待决，再按键；
                           // 面板可见期间吞掉其余按键，避免落入隐藏的命令输入。
                           sync_decision();
                           if (decision_panel_.visible())
                           {
                               // 命令输入已隐藏，q 无输入冲突：待决中仍可退出。
                               if (event == ftxui::Event::q)
                               {
                                   controller_.request_quit();
                                   return true;
                               }
                               decision_panel_.on_event(event);
                               return true;
                           }

                            // 无待决：日志滚动键优先于命令输入。按键映射为纯语义
                            // LogKey 后交纯状态机判定，Home/End 仅在输入缓冲为空时
                            // 接管，避免抢占命令编辑的光标键；PgUp/PgDn 无条件。
                            LogKey key = LogKey::None;
                            if (event == ftxui::Event::PageUp)
                                key = LogKey::PageUp;
                            else if (event == ftxui::Event::PageDown)
                                key = LogKey::PageDown;
                            else if (event == ftxui::Event::Home)
                                key = LogKey::Home;
                            else if (event == ftxui::Event::End)
                                key = LogKey::End;
                            return handle_log_key(
                                key, command_input_.empty(), log_scroll_,
                                controller_.log_lines().size());
                        });
        }
    }  // namespace tui
}  // namespace tkw
