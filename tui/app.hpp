/**
 * @file app.hpp
 * @brief TUI 应用壳：持有一局真实会话、日志缓冲与值快照，渲染四面板。
 * @note FTXUI 类型只出现在本头与实现中，纯视图模型仍由 tkw_tui 提供；成员声明
 *       顺序即析构保证：log_ 后声明先析构，日志订阅句柄在 session_ 的 Game 之前
 *       退订，不会向已释放总线退订。
 */
#ifndef INCLUDE_TKW_TUI_APP_HPP
#define INCLUDE_TKW_TUI_APP_HPP

#include <string>

#include <ftxui/ftxui.hpp>

#include "cli/session.hpp"
#include "tui/log_lines.hpp"
#include "tui/snapshot.hpp"

namespace tkw
{
    namespace tui
    {
        /**
         * @class App
         * @brief TUI 壳：持有一局真实会话、日志缓冲与值快照，渲染四面板。
         * @note 只做「建局 → 快照 → 渲染」；命令驱动、后台线程与退出存档由后续任务接入。
         */
        class App
        {
        public:
            /**
             * @brief 建默认对局、绑日志、建首帧快照。
             * @note 日志订阅先于开局发牌建立，初始摸牌事件才会落入日志缓冲；
             *       牌表加载或开局失败只记录提示并保持空快照，不阻断渲染。
             */
            void bootstrap();

            /** @brief 重建值快照（值拷贝）；渲染与后续任务在 Post 回调中调用。 */
            void refresh();

            /**
             * @brief 组装根组件：Renderer(render) + q/Esc/Ctrl-C 退出。
             * @param screen 全屏屏幕；退出事件调用其 Exit()。
             */
            ftxui::Component component(ftxui::ScreenInteractive &screen);

        private:
            /** @brief 组装整屏 DOM：棋盘 / 手牌 / 日志 / 状态四面板 + 提示行。 */
            ftxui::Element render() const;

            cli::Session session_;  /**< 先声明：Game 生命周期最长 */
            LogBuffer log_;         /**< 后声明：析构先于 session_，退订时总线仍活 */
            UiSnapshot snapshot_;   /**< 值快照，脱 Game 生命周期 */
            std::string viewer_ = "P0"; /**< 视角座位：己方手牌展开，他人只出数量 */
            std::string notice_;    /**< 提示行文案（建局失败/按键提示） */
        };
    }  // namespace tui
}  // namespace tkw

#endif  // INCLUDE_TKW_TUI_APP_HPP
