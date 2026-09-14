/**
 * @file render.hpp
 * @brief TUI 渲染层：把纯值快照/日志组装为 FTXUI DOM，并按终端尺寸分级降级。
 * @note 纯函数渲染：同一 ShellSpec 产出同一 DOM，可用固定尺寸 Screen 单测。
 *       只依赖 FTXUI 与 src/tui 的纯值模型，不触引擎容器、控制器与终端读写；
 *       FTXUI 类型只出现在本层与 tkw-tui 目标。
 */
#ifndef INCLUDE_TKW_TUI_RENDER_HPP
#define INCLUDE_TKW_TUI_RENDER_HPP

#include <cstdint>
#include <string>
#include <vector>

#include <ftxui/ftxui.hpp>

#include "tui/snapshot.hpp"

namespace tkw
{
    namespace tui
    {
        namespace detail
        {
            /** @brief 布局分级：按终端尺寸决定展示哪些面板。 */
            enum class LayoutMode : std::uint8_t
            {
                Full,    /**< 棋盘 + 手牌/日志 + 状态 + 底部区 */
                Compact, /**< 日志 + 状态 + 底部区（隐藏棋盘与手牌） */
                Minimal, /**< 日志 + 底部区（隐藏棋盘、手牌与状态） */
            };

            /**
             * @brief 按终端尺寸挑选布局分级。
             * @param[in] size 终端行列；异常尺寸由调用方回退后再传入。
             * @param[in] bottom_expanded 底部是否为展开的决策面板；展开时让出中上部空间。
             * @return 尺寸达标（展开面板时需更多行数）→ Full；行数够 → Compact；
             *         否则 Minimal。
             */
            LayoutMode plan_layout(ftxui::Dimensions size, bool bottom_expanded);

            /** @brief 整屏渲染输入：值引用须在 render_shell 调用期间存活。 */
            struct ShellSpec
            {
                const UiSnapshot &snap;                    /**< 四面板值快照 */
                const std::vector<std::string> &log_lines; /**< 日志值拷贝 */
                const std::string &notice;                 /**< 底部提示行文案 */
                ftxui::Element bottom;                     /**< 命令输入行或决策面板 */
                ftxui::Dimensions size;                    /**< 终端行列 */
                float log_ratio = 1.0f;                    /**< 日志视口 0=顶 1=尾 */
                bool bottom_expanded = false;              /**< 决策面板是否展开 */
            };

            /**
             * @brief 组装整屏 DOM：按布局分级挑选面板，中段吸收富余/亏损。
             * @param[in] spec 渲染输入。
             * @return FTXUI 根元素；底部（命令输入/决策面板）始终保留。
             * @note Full 的中段 hbox 带 flex，日志内容进可滚动视口：日志行数变化
             *       不改变棋盘/状态/底部行位；Compact/Minimal 追加降级提示。
             */
            ftxui::Element render_shell(const ShellSpec &spec);

            /** @brief 棋盘面板：每座体力/手牌数/装备/判定/距离（身份局附角色）。 */
            ftxui::Element render_board(const UiSnapshot &snap);

            /** @brief 手牌面板：viewer 己方手牌展开，他人只出数量。 */
            ftxui::Element render_hand(const UiSnapshot &snap);

            /** @brief 状态面板：会话进度 / AI 档 / 牌堆规模 / 模式与终局。 */
            ftxui::Element render_status(const UiSnapshot &snap);

            /**
             * @brief 日志面板：全部行放进可滚动视口。
             * @param[in] lines 日志值拷贝；空则单行占位。
             * @param[in] ratio 视口位置 0=顶 1=尾；内容溢出时显示滚动条。
             */
            ftxui::Element render_log(const std::vector<std::string> &lines,
                                      float ratio);
        }  // namespace detail
    }  // namespace tui
}  // namespace tkw

#endif  // INCLUDE_TKW_TUI_RENDER_HPP
