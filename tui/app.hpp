/**
 * @file app.hpp
 * @brief TUI 应用壳：FTXUI 适配层，持控制器、命令输入框与决策面板，渲染四面板。
 * @note FTXUI 类型只出现在本头与实现中；会话驱动、后台线程、日志与退出存档
 *       全在 Controller（无 FTXUI、可单测）。成员声明序即析构保证：命令输入
 *       组件先于输入缓冲析构，不持悬空引用。
 */
#ifndef INCLUDE_TKW_TUI_APP_HPP
#define INCLUDE_TKW_TUI_APP_HPP

#include <cstddef>
#include <string>
#include <utility>

#include <ftxui/ftxui.hpp>

#include "decision_panel.hpp"
#include "tui/controller.hpp"

namespace tkw
{
    namespace tui
    {
        /**
         * @class App
         * @brief TUI 壳：把 FTXUI 组件、屏幕回送接缝、命令输入与决策面板接到 Controller。
         * @note 日志滚动状态只由 UI 主线程读写，不影响 Container 焦点链；渲染参数
         *       （布局分级、滚动位置）都在 render() 内折成纯值交给渲染层。
         */
        class App
        {
        public:
            /** @brief 构造应用壳（不建局；建局在 bootstrap）。 */
            App() = default;

            /** @brief 建默认对局并起首帧（转调控制器）。 */
            void bootstrap();

            /**
             * @brief 设置启动选项基准（argv 透传）；bootstrap 前调用生效。
             * @param options 命令行解析出的选项；未给出的项沿用结构体默认值。
             */
            void set_base_options(tkw::cli::Options options)
            {
                controller_.set_base_options(std::move(options));
            }

            /**
             * @brief 组装根组件：命令输入 + 渲染器 + 决策面板键位 + Esc/Ctrl-C 退出。
             * @param screen 全屏屏幕；注入 Post 与退出回调都由其承载。
             * @note 待决期面板覆盖命令输入并接管键位（↑/↓/Enter/p/空格/数字，
             *       q 退出）；无待决时按键回落命令输入。退出由控制器 request_quit
             *       统一处理（取消 → join → 存档 → Exit），Esc/Ctrl-C 不再直接
             *       调用 screen.Exit()。
             */
            ftxui::Component component(ftxui::ScreenInteractive &screen);

            /** @brief 退出自动存档信息；入口在事件循环结束后写 stderr。 */
            const std::string &exit_message() const { return controller_.exit_message(); }

        private:
            /** @brief 组装整屏 DOM：四面板 + 命令输入行或决策面板。 */
            ftxui::Element render() const;

            /** @brief 有待决且面板未显示时取走面板；每个待决只取一次。 */
            void sync_decision();

            /**
             * @brief 日志视口相对位置。
             * @return 跟随末尾或行数 ≤1 时恒 1；否则锚定行下标占总行数的比例。
             */
            float log_ratio() const;

            /**
             * @brief 按行滚动日志视口；越界钳位，滚到尾部恢复跟随。
             * @param delta 正数向尾部、负数向顶部移动的行数。
             */
            void scroll_log(int delta);

            Controller controller_;      /**< 会话驱动、worker、日志与存档 */
            std::string command_input_;  /**< 命令输入缓冲（先声明，后于输入组件析构） */
            ftxui::Component input_;     /**< 命令输入框 */
            DecisionPanel decision_panel_; /**< 真人待决面板（覆盖输入行） */
            std::string notice_;         /**< 底部提示行文案 */
            bool log_follow_ = true;     /**< 日志视口是否贴尾 */
            std::size_t log_anchor_ = 0; /**< 非跟随时锚定的行下标（0=顶） */
        };
    }  // namespace tui
}  // namespace tkw

#endif  // INCLUDE_TKW_TUI_APP_HPP
