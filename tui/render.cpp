/**
 * @file render.cpp
 * @brief TUI 渲染实现：四面板 DOM 组装、中段 flex 布局与日志滚动视口。
 * @note 只读纯值模型；布局分级与降级提示在此单点决定，App 只负责准备底部区。
 */

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <ftxui/ftxui.hpp>

#include "render.hpp"
#include "card/def.hpp"
#include "cli/render.hpp"
#include "game/core/roles.hpp"
#include "tui/command.hpp"

namespace
{
    using tkw::tui::UiSnapshot;
    using tkw::tui::detail::LayoutMode;

    /** Full 模式的最小终端行列。 */
    constexpr int kFullMinRows = 24;
    constexpr int kFullMinCols = 80;
    /** Compact 模式的最小终端行数；更小进 Minimal。 */
    constexpr int kCompactMinRows = 12;
    /** 极端兜底：小于此行数只保留底部交互区。 */
    constexpr int kTinyRows = 5;
    /** 棋盘/状态面板行数上限，避免多玩家局挤占中段。 */
    constexpr int kBoardMaxRows = 12;
    constexpr int kStatusMaxRows = 6;

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

    /** @brief 降级提示文案：按模式说明已被隐藏的面板。 */
    std::string degradation_notice(const ftxui::Dimensions &size, LayoutMode mode)
    {
        const std::string hidden = mode == LayoutMode::Minimal
                                       ? "已隐藏棋盘、手牌与状态"
                                       : "已隐藏棋盘与手牌";
        return "终端过小（" + std::to_string(size.dimx) + "×" +
               std::to_string(size.dimy) + "，建议 ≥80×24）：" + hidden;
    }
}  // namespace

namespace tkw
{
    namespace tui
    {
        namespace detail
        {
            LayoutMode plan_layout(ftxui::Dimensions size, bool bottom_expanded)
            {
                if (!bottom_expanded && size.dimy >= kFullMinRows &&
                    size.dimx >= kFullMinCols)
                    return LayoutMode::Full;
                if (size.dimy >= kCompactMinRows)
                    return LayoutMode::Compact;
                return LayoutMode::Minimal;
            }

            /**
             * @brief 棋盘面板：每座一行体力/手牌数/装备/判定/距离，身份局附加角色标签。
             * @param snap 值快照。
             * @return 每座位一行的 vbox；无玩家时单行占位。
             * @note 装备区/判定区为明置信息，经快照值展开牌名，空区回落「无」，与 CLI
             *       status 同措辞；手牌仍只出数量。身份局的隐藏座位已在快照层收敛为
             *       Role::None，此处无条件输出其标签（None → 「未知」占位），与 CLI
             *       status 口径一致，不泄漏真实角色。
             */
            ftxui::Element render_board(const UiSnapshot &snap)
            {
                std::vector<ftxui::Element> rows;
                const bool show_role =
                    snap.mode == tkw::game::GameMode::Identity;

                for (const auto &p : snap.players)
                {
                    std::string line = "P" + std::to_string(p.seat) + "  体力 " +
                                       std::to_string(p.hp) + "/" +
                                       std::to_string(p.max_hp) + "  手牌 " +
                                       std::to_string(p.hand.count) + "  装备 " +
                                       tkw::tui::zone_names(p.equip) + "  判定 " +
                                       tkw::tui::zone_names(p.judge) + "  距 " +
                                       std::to_string(p.distance);
                    if (p.in_attack_range)
                        line += "  攻击范围";
                    if (show_role)
                        line += "  [" +
                                std::string(tkw::cli::detail::role_label_zh(p.role)) +
                                "]";
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
                    return ftxui::text("手牌 " +
                                       std::to_string(viewer->hand.count) +
                                       " 张（不可见）");
                if (viewer->hand.cards.empty())
                    return ftxui::text("（无手牌）");

                std::vector<ftxui::Element> rows;
                for (std::size_t i = 0; i < viewer->hand.cards.size(); ++i)
                {
                    const auto &card = viewer->hand.cards[i];
                    std::string line = std::to_string(i + 1) + ". " +
                                       card.display_name + "  " +
                                       suit_glyph(card.suit) +
                                       std::to_string(card.number);
                    rows.push_back(ftxui::text(line));
                }
                return ftxui::vbox(std::move(rows));
            }

            /**
             * @brief 日志面板：全部行进可滚动视口。
             * @param lines 控制器回送的日志值拷贝。
             * @param ratio 视口位置 0=顶 1=尾；内容溢出时显示滚动条。
             * @return 带纵向滚动条的可滚动 vbox；无内容时单行占位。
             * @note 不引入可聚焦组件：focusPositionRelative 只写焦点位置，不参与
             *       Container 焦点链，命令输入与决策面板的键位归属不受影响。
             */
            ftxui::Element render_log(const std::vector<std::string> &lines,
                                      float ratio)
            {
                if (lines.empty())
                    return ftxui::text("（暂无日志）");

                std::vector<ftxui::Element> rows;
                rows.reserve(lines.size());
                for (const auto &line : lines)
                    rows.push_back(ftxui::text(line));

                return ftxui::vbox(std::move(rows)) |
                       ftxui::focusPositionRelative(
                           0.f, std::clamp(ratio, 0.f, 1.f)) |
                       ftxui::vscroll_indicator | ftxui::yframe;
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
                    rows.push_back(ftxui::text("会话: 已结束  胜者: " +
                                               snap.winner_label));
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

            ftxui::Element render_shell(const ShellSpec &spec)
            {
                const LayoutMode mode = plan_layout(spec.size, spec.bottom_expanded);
                const ftxui::Element notice =
                    ftxui::text(spec.notice) | ftxui::dim;

                // 极端兜底：空间只够底部交互区时仅渲染底部，保证命令输入不被裁掉。
                if (mode == LayoutMode::Minimal && spec.size.dimy < kTinyRows)
                    return ftxui::vbox({spec.bottom});

                const ftxui::Element log_panel =
                    panel("日志", render_log(spec.log_lines, spec.log_ratio));
                const ftxui::Element degrade =
                    ftxui::text(degradation_notice(spec.size, mode)) | ftxui::dim;

                if (mode == LayoutMode::Full)
                {
                    const ftxui::Element board =
                        panel("棋盘", render_board(spec.snap)) |
                        ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN,
                                    kBoardMaxRows);
                    const ftxui::Element hand =
                        panel("手牌", render_hand(spec.snap)) | ftxui::flex;
                    const ftxui::Element status =
                        panel("状态", render_status(spec.snap)) |
                        ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN,
                                    kStatusMaxRows);

                    // 中段 hbox 带 flex：吸收整屏富余/亏损，棋盘/状态/底部行位
                    // 不随日志行数浮动；日志内容进 yframe 后行数不改变面板外高。
                    return ftxui::vbox({
                        board,
                        ftxui::hbox({hand, log_panel | ftxui::flex}) |
                            ftxui::flex,
                        status,
                        spec.bottom,
                        notice,
                    });
                }

                if (mode == LayoutMode::Compact)
                {
                    const ftxui::Element status =
                        panel("状态", render_status(spec.snap)) |
                        ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN,
                                    kStatusMaxRows);
                    return ftxui::vbox({
                        log_panel | ftxui::flex,
                        status,
                        spec.bottom,
                        degrade,
                        notice,
                    });
                }

                return ftxui::vbox({
                    log_panel | ftxui::flex,
                    spec.bottom,
                    degrade,
                    notice,
                });
            }
        }  // namespace detail
    }  // namespace tui
}  // namespace tkw
