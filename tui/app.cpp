/**
 * @file app.cpp
 * @brief TUI 壳实现：建默认真实对局、四面板渲染与退出组件。
 * @note 四面板数据全部来自 tkw_tui 的值快照与日志缓冲，渲染层不触引擎容器；
 *       中文/fullwidth 直接交给 FTXUI 计宽，不做按字节对齐。
 */

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <ftxui/ftxui.hpp>

#include "app.hpp"
#include "cli/render.hpp"
#include "game/core/roles.hpp"
#include "game/flow/factory.hpp"
#include "game/flow/loop.hpp"

namespace
{
    using tkw::tui::LogBuffer;
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

    /** @brief AI 难度档 → 展示字符串；与命令行/存档值域同名。 */
    const char *ai_level_zh(tkw::cli::AiLevel ai)
    {
        return ai == tkw::cli::AiLevel::Aggressive ? "aggressive" : "simple";
    }

    /**
     * @brief 棋盘面板：每座一行体力/手牌数/距离，身份局附加角色标签。
     * @param snap 值快照。
     * @return 每座位一行的 vbox；无玩家时单行占位。
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
            if (show_role && p.role != tkw::game::Role::None)
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
     * @brief 日志面板：取缓冲末段真实事件行。
     * @param log 日志缓冲。
     * @return 末 kLogRows 行的 vbox；无内容时单行占位。
     */
    ftxui::Element render_log(const LogBuffer &log)
    {
        const auto &lines = log.lines();
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
                snap.current + "  AI: " + ai_level_zh(snap.ai) + "  摸牌堆 " +
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
     * @brief 组装整屏 DOM：棋盘 / 手牌与日志 / 状态 / 提示行。
     * @param snap 值快照。
     * @param log 日志缓冲。
     * @param notice 底部提示文案。
     */
    ftxui::Element render_shell(const UiSnapshot &snap, const LogBuffer &log,
                                const std::string &notice)
    {
        auto board = panel("棋盘", render_board(snap));
        auto hand = panel("手牌", render_hand(snap));
        auto log_panel = panel("日志", render_log(log));
        auto status = panel("状态", render_status(snap));

        return ftxui::vbox({
            std::move(board),
            ftxui::hbox({std::move(hand) | ftxui::flex,
                         std::move(log_panel) | ftxui::flex}),
            std::move(status),
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
            notice_ = "q / Esc / Ctrl-C 退出";

            tkw::cli::Options opt;
            auto built = tkw::game::build_game(tkw::game::BuildOptions{
                opt.deck, opt.players, opt.seed, opt.mode});
            if (built.is_err())
            {
                notice_ = "建局失败：请确认在仓库根目录运行（牌表 resources/）";
                return;
            }

            session_.game = std::move(built).unwrap();
            session_.deck = opt.deck;
            session_.ai = opt.ai;

            // 日志订阅先于开局发牌建立，初始摸牌事件才会进入面板。
            log_.bind(*session_.game);

            auto ctx = session_.game->context();
            auto started =
                tkw::game::start_session(ctx, session_.state, "P0", opt.hand);
            if (started.is_err())
            {
                notice_ = "开局失败：场上没有玩家";
                log_.unbind();
                session_.game.reset();
                return;
            }

            session_.active = true;
            refresh();
        }

        void App::refresh()
        {
            snapshot_ = make_snapshot(session_, viewer_);
        }

        ftxui::Element App::render() const
        {
            return render_shell(snapshot_, log_, notice_);
        }

        ftxui::Component App::component(ftxui::ScreenInteractive &screen)
        {
            return ftxui::Renderer([this] { return render(); }) |
                   ftxui::CatchEvent(
                       [&screen](ftxui::Event event)
                       {
                           if (event == ftxui::Event::Character('q') ||
                               event == ftxui::Event::Escape ||
                               event == ftxui::Event::CtrlC)
                           {
                               screen.Exit();
                               return true;
                           }
                           return false;
                       });
        }
    }  // namespace tui
}  // namespace tkw
