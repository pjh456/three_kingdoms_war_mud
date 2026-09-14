/**
 * @file   hp.hpp
 * @brief  血条模型：体力 cur / 上限 max。
 * @details 扣到非正是濒死值状态，合法且可继续被桃拉回；死亡不是状态值问题，
 *          是 combat 的流程判定。
 * @ingroup tkw_entity
 */

#ifndef INCLUDE_TKW_ENTITY_HP_HPP
#define INCLUDE_TKW_ENTITY_HP_HPP

#include <algorithm>
#include <functional>

namespace tkw
{
    namespace entity
    {
        /**
         * @brief  血条（体力 / 体力上限）。
         * @details 所有修改路径收敛到 `set_cur`/`set_max`，值真正变化时触发
         *          `on_change` 回调（无变化不触发）；`add` 受上限钳制，`sub`
         *          无下限——扣到非正即濒死值状态；`add`/`sub` 返回实际变化量
         *          （受钳制，可能小于入参或为 0）。
         */
        class Hp
        {
        private:
            int m_cur = 0;
            int m_max = 0;
            std::function<void(int, int, int)> m_on_change;  // (old_cur, cur, max)

        public:
            /**
             * @brief  构造初始满血血条。
             * @param[in] initial 初始体力，同时作为上限；`cur = max = initial`。
             * @return 满血的 `Hp` 值对象。
             */
            static Hp make(int initial);

            /**
             * @brief  返回当前体力。
             * @return 当前体力值；可为非正 = 濒死值状态。
             */
            int get_cur() const noexcept { return m_cur; }

            /**
             * @brief  返回体力上限。
             * @return 当前体力上限。
             */
            int get_max() const noexcept { return m_max; }

            /**
             * @brief  直接设置体力。
             * @param[in] val 目标体力；`val > m_max` 拒绝，`val < 0` 合法
             *                （濒死值状态）。
             * @return 是否设置成功。
             * @retval true  `val <= m_max`（含与当前值相同、无回调触发）。
             * @retval false `val > m_max`，状态不变。
             * @post   若 `val` 与当前值不同且已注册回调，则触发一次 `on_change`。
             */
            bool set_cur(int val);

            /**
             * @brief  变更体力上限。
             * @param[in] val 目标上限；`val < 0` 拒绝。
             * @return 是否设置成功。
             * @retval true  `val >= 0`。
             * @retval false `val < 0`，状态不变。
             * @post   若 `m_cur` 超过新上限则夹紧为上限；仅就 `m_cur` 的变化触发
             *          一次回调（上限自身变化不触发）。
             */
            bool set_max(int val);

            /**
             * @brief  增加体力（受上限钳制）。
             * @param[in] det 期望增加量。
             * @return 实际增加量（可能小于 `det` 或为 0）。
             */
            int add(int det);

            /**
             * @brief  减少体力（可为非正 = 濒死值状态）。
             * @param[in] det 期望减少量。
             * @return 实际减少量。
             */
            int sub(int det);

            /**
             * @brief  注册体力变化回调。
             * @param[in] cb 回调 `(old_cur, cur, max)`；覆盖式注册，仅保留最近一个。
             * @post   此后 `m_cur` 的任何实际变化都会以新值回调一次。
             * @note   传入空 `std::function` 可清除已有回调。
             */
            void on_change(std::function<void(int, int, int)> cb)
            {
                m_on_change = std::move(cb);
            }
        };
    }
}

#endif  // INCLUDE_TKW_ENTITY_HP_HPP
