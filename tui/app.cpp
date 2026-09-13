/**
 * @file app.cpp
 * @brief TUI 壳实现：四面板渲染、命令输入与真人决策面板，数据全部来自控制器值模型。
 * @note 渲染层不触引擎容器：快照与日志都是控制器回送的值拷贝，worker 运行
 *       期间主线程只读模型。屏幕回送经 screen.Post（FTXUI TaskQueue 自带锁），
 *       闭包只捕获模型 shared_ptr 与值，不捕获 App。
 */

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <ftxui/ftxui.hpp>

#include "app.hpp"
#include "cli/render.hpp"
#include "game/core/roles.hpp"

namespace
{
    using tkw::tui::UiSnapshot;

    /** 日志面板最多展示的行数。 */
    constexpr std::size_t kLogRows = 18;

    /**
     * @brief 单个带圆角边框与粗体标题的面板。
     * @param title 面板标题。
     * @param body 面板内容。
     * @return 加边框的 FTXUI 元素。
     */
    ftxui::Element panel(const std::string &title, ftxui::Element body)
    {
        return ftxui::window(ftxui::text(title) | ftxui::bold, std::move(body));
    }

    /** @brief 花色 → UTF-8 符号；仅作行内装饰，不参与跨行对齐。 */
    const char *suit_glyph(tkw::card::Suit suit)
    {
        switch (suit)
        {
        case tkw::card::Suit::Spade:
            return "♠";
        case tkw::card::Suit::Club:
            return "♣";
        case tkw::card::Suit::Heart:
            return "♥";
        case tkw::card::Suit::Diamond:
            return "♦";
        default:
            return "?";
        }
    }

    /**
     * @brief 棋盘面板：每座一行体力/手牌数/距离，身份局附加角色标签。
     * @param snap 值快照。
     * @return 每座位一行的 vbox；无玩家时单行占位。
     * @note 身份局的隐藏座位已在快照层收敛为 Role::None，此处无条件输出其标签
     *       （None → 「未知」占位），与 CLI status 口径一致，不泄漏真实角色。
     */
    ftxui::Element render_board(const UiSnapshot &snap)
    {
        std::vector<ftxui::Element> rows;
        const bool show_role = snap.mode == tkw::game::GameMode::Identity;

        for (const auto &p : snap.players)
        {
            std::string line = "P" + std::to_string(p.seat) + "  体力 " +
                               std::to_string(p.hp) + "/" +
                               std::to_string(p.max_hp) + "  手牌 " +
                               std::to_string(p.hand.count) + "  距 " +
                               std::to_string(p.distance);
            if (p.in_attack_range)
                line += "  攻击范围";
            if (show_role)
                line += "  [" + std::string(tkw::cli::detail::role_label_zh(p.role)) + "]";
            rows.push_back(ftxui::text(line));
        }

        if (rows.empty())
            rows.push_back(ftxui::text("（无玩家）"));
        return ftxui::vbox(std::move(rows));
    }

    /**
     * @brief 手牌面板：viewer 视角的己方手牌逐张展开，他人只出数量。
     * @param snap 值快照。
     * @return 手牌行列表；对手视角或空手牌时单行占位。
     */
    ftxui::Element render_hand(const UiSnapshot &snap)
    {
        const tkw::tui::PlayerRow *viewer = nullptr;
        for (const auto &p : snap.players)
        {
            if (p.id == snap.viewer)
            {
                viewer = &p;
                break;
            }
        }

        if (viewer == nullptr)
            return ftxui::text("（无手牌）");
        if (!viewer->hand.revealed)
            return ftxui::text("手牌 " + std::to_string(viewer->hand.count) +
                               " 张（不可见）");
        if (viewer->hand.cards.empty())
            return ftxui::text("（无手牌）");

        std::vector<ftxui::Element> rows;
        for (std::size_t i = 0; i < viewer->hand.cards.size(); ++i)
        {
            const auto &card = viewer->hand.cards[i];
            std::string line = std::to_string(i + 1) + ". " + card.display_name +
                               "  " + suit_glyph(card.suit) +
                               std::to_string(card.number);
            rows.push_back(ftxui::text(line));
        }
        return ftxui::vbox(std::move(rows));
    }

    /**
     * @brief 日志面板：取值拷贝末段真实事件行。
     * @param lines 控制器回送的日志值拷贝。
     * @return 末 kLogRows 行的 vbox；无内容时单行占位。
     */
    ftxui::Element render_log(const std::vector<std::string> &lines)
    {
        if (lines.empty())
            return ftxui::text("（暂无日志）");

        const std::size_t begin =
            lines.size() > kLogRows ? lines.size() - kLogRows : 0;
        std::vector<ftxui::Element> rows;
        for (std::size_t i = begin; i < lines.size(); ++i)
            rows.push_back(ftxui::text(lines[i]));
        return ftxui::vbox(std::move(rows));
    }

    /** @brief 真人座位 id 串：无真人时为空串。 */
    std::string join_humans(const std::vector<std::string> &humans)
    {
        std::string joined;
        for (std::size_t i = 0; i < humans.size(); ++i)
        {
            if (i != 0)
                joined += ",";
            joined += humans[i];
        }
        return joined;
    }

    /**
     * @brief 状态面板：会话进度 / AI 档 / 牌堆规模 / 模式与终局。
     * @param snap 值快照。
     * @return 状态行列表。
     */
    ftxui::Element render_status(const UiSnapshot &snap)
    {
        std::vector<ftxui::Element> rows;

        if (!snap.active)
            rows.push_back(ftxui::text("没有进行中的对局"));
        else if (snap.over)
            rows.push_back(ftxui::text("会话: 已结束  胜者: " + snap.winner_label));
        else
            rows.push_back(ftxui::text(
                "第 " + std::to_string(snap.turns) + " 回合  下一回合: " +
                snap.current + "  AI: " +
                tkw::tui::detail::ai_level_name(snap.ai) + "  摸牌堆 " +
                std::to_string(snap.draw_size) + "  弃牌堆 " +
                std::to_string(snap.discard_size)));

        if (snap.active)
        {
            const std::string mode_zh =
                snap.mode == tkw::game::GameMode::Brawl ? "乱斗" : "身份局";
            rows.push_back(ftxui::text("模式: " + mode_zh + "  牌表: " +
                                       snap.deck.string()));
            const std::string humans = join_humans(snap.humans);
            if (!humans.empty())
                rows.push_back(ftxui::text("真人座: " + humans));
            if (snap.at_cap)
                rows.push_back(ftxui::text("已达回合上限"));
        }

        return ftxui::vbox(std::move(rows));
    }

    /**
     * @brief 组装整屏 DOM：棋盘 / 手牌与日志 / 状态 / 底部交互区。
     * @param snap 值快照。
     * @param log_lines 日志值拷贝。
     * @param notice 底部提示文案。
     * @param bottom 底部交互区：命令输入行或真人决策面板。
     */
    ftxui::Element render_shell(const UiSnapshot &snap,
                                const std::vector<std::string> &log_lines,
                                const std::string &notice,
                                ftxui::Element bottom)
    {
        auto board = panel("棋盘", render_board(snap));
        auto hand = panel("手牌", render_hand(snap));
        auto log_panel = panel("日志", render_log(log_lines));
        auto status = panel("状态", render_status(snap));

        return ftxui::vbox({
            std::move(board),
            ftxui::hbox({std::move(hand) | ftxui::flex,
                         std::move(log_panel) | ftxui::flex}),
            std::move(status),
            std::move(bottom),
            ftxui::text(notice) | ftxui::dim,
        });
    }
}  // namespace

namespace tkw
{
    namespace tui
    {
        void App::bootstrap()
        {
            notice_ = "new / deal / step / run / status / save / load / quit；"
                      "help 查看用法，Esc / Ctrl-C 退出";
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

            return render_shell(controller_.snapshot(), controller_.log_lines(),
                                notice_, std::move(bottom));
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
            input_option.placeholder = "输入命令（help 查看用法）";
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
                           return false;
                       });
        }
    }  // namespace tui
}  // namespace tkw
