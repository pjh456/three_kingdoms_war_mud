/**
 * @file decision_panel.hpp
 * @brief FTXUI 真人决策面板：只读纯值面板视图，产出选中下标与放弃标记。
 * @note 面板不含引擎逻辑与目录访问：候选文本、多选数量与 pass 语义都由 worker
 *       事前折好；本类只维护光标/勾选状态并把确认结果经回调回送。Esc/Ctrl-C 不
 *       在此处理，由应用壳统一全局退出。
 */
#ifndef INCLUDE_TKW_TUI_DECISION_PANEL_HPP
#define INCLUDE_TKW_TUI_DECISION_PANEL_HPP

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <ftxui/ftxui.hpp>

#include "tui/decision_source.hpp"

namespace tkw
{
    namespace tui
    {
        /**
         * @class DecisionPanel
         * @brief 待决决策的交互面板：↑/↓ 选项、Enter 确认、p 放弃、空格多选、数字直选。
         * @note 每次 show() 重置光标与多选；只在面板可见时调用 on_event/render。
         */
        class DecisionPanel
        {
        public:
            /** 提交回调：参数为选中下标与 pass；返回 true 表示已被接受（面板关闭）。 */
            using Submit = std::function<bool(std::vector<std::size_t>, bool)>;

            /** @brief 设置提交回调；未设置时确认操作只提示失败。 */
            void set_on_submit(Submit submit) { on_submit_ = std::move(submit); }

            /**
             * @brief 接收新的待决面板并重置选择状态。
             * @param view 纯值面板视图；候选已在 worker 侧解析成展示文本。
             */
            void show(DecisionPanelView view);

            /** @brief 关闭面板并清除选择状态。 */
            void hide();

            /** @brief 面板是否可见。 */
            bool visible() const noexcept { return view_.has_value(); }

            /**
             * @brief 处理一个按键。
             * @return 已消费返回 true；未识别的按键返回 false，由上层决定是否吞掉。
             */
            bool on_event(const ftxui::Event &event);

            /** @brief 渲染面板主体：候选项、光标/勾选与键位提示。 */
            ftxui::Element render() const;

        private:
            /** @brief 提交当前选择；多选数量不符或无回调时只写提示、保持待决。 */
            void confirm();

            /** @brief 放弃当前决策；allow_pass 为假时只写提示。 */
            void discard();

            /** @brief 移动光标；空候选不动作。 */
            void move_cursor(int delta);

            /** @brief 多选：切换光标处勾选态。 */
            void toggle_cursor();

            /**
             * @brief 当前选中下标：多选取勾选集合，单选取光标。
             * @return 下标按升序；无候选时为空。
             */
            std::vector<std::size_t> selected() const;

            std::optional<DecisionPanelView> view_; /**< 当前待决；空 = 不可见 */
            std::size_t cursor_ = 0;                /**< 单/多选共有光标 */
            std::vector<bool> checked_;             /**< 多选勾选态 */
            std::string notice_;                    /**< 非法操作提示 */
            Submit on_submit_;                      /**< 提交接缝 */
        };
    }  // namespace tui
}  // namespace tkw

#endif  // INCLUDE_TKW_TUI_DECISION_PANEL_HPP
