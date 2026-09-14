/**
 * @file   rng.hpp
 * @brief  随机源抽象：把引擎从 `std::mt19937` 解绑，便于注入确定性/可记录的序列。
 * @details 接口暴露 `next()`（32 位均匀）与 `save_state`/`load_state`（存档续档用）；
 *          洗牌等消费方用 `uniform_below` 做拒绝采样，保证无取模偏差。
 * @ingroup tkw_util
 */

#ifndef INCLUDE_TKW_UTIL_RNG_HPP
#define INCLUDE_TKW_UTIL_RNG_HPP

#include <cstdint>
#include <ios>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace tkw
{
    /**
     * @brief 随机源可序列化状态。
     * @note data 为实现定义的文本串（同版本内跨平台稳定）；空串 = 未初始化。
     */
    struct RngState
    {
        std::string data; /**< 状态文本；空串表示未初始化。 */

        /**
         * @brief  比较两个状态是否等价。
         * @return `true` 表示 `data` 相等。
         */
        bool operator==(const RngState &) const = default;
    };

    /** @brief 随机源接口（对局随机性的唯一入口）。 */
    struct Rng
    {
        /** @brief 虚析构：允许经基类指针销毁具体实现。 */
        virtual ~Rng() = default;

        /**
         * @brief  下一个 32 位均匀随机数。
         * @return 均匀分布的 32 位无符号整数。
         */
        virtual std::uint32_t next() = 0;

        /**
         * @brief  导出当前状态（存档用）。
         * @return 序列化后的状态快照。
         */
        virtual RngState save_state() const = 0;

        /**
         * @brief  恢复状态。
         * @param[in] state 待恢复的状态快照。
         * @return `true` 表示恢复成功。
         * @retval true  引擎采用 `state`。
         * @retval false `state.data` 非法，引擎保持原状。
         */
        virtual bool load_state(const RngState &state) = 0;
    };

    /** @brief 种子化 mt19937 实现（生产与测试默认）。 */
    class SeededRng : public Rng
    {
    public:
        /**
         * @brief  以种子构造。
         * @param[in] seed 初始种子。
         */
        explicit SeededRng(std::uint32_t seed) : m_engine(seed) {}

        /**
         * @brief  取自 `mt19937` 的下一个值。
         * @return 底层引擎产生的 32 位无符号整数。
         */
        std::uint32_t next() override { return m_engine(); }

        /**
         * @brief  导出 `mt19937` 当前状态。
         * @return 序列化后的状态快照。
         */
        RngState save_state() const override
        {
            std::ostringstream os;
            os << m_engine;
            return RngState{os.str()};
        }

        /**
         * @brief  恢复 `mt19937` 引擎状态。
         * @param[in] state 待恢复的状态快照。
         * @return `true` 表示恢复成功。
         * @retval true  引擎采用 `state`。
         * @retval false `state.data` 非法，引擎保持原状。
         * @note   非法文本的异常类型各标准库不同，此处统一归为失败。
         */
        bool load_state(const RngState &state) override
        {
            std::istringstream is(state.data);
            std::mt19937 restored;
            // 非法文本的处理各标准库不同：libstdc++ 抛 ios_base::failure，
            // MSVC 抛 invalid_argument，也有实现只置 failbit。
            // 全部归为「data 非法」，失败时引擎保持原状。
            try
            {
                is >> restored;
            }
            catch (const std::ios_base::failure &)
            {
                return false;
            }
            catch (const std::invalid_argument &)
            {
                return false;
            }
            if (is.fail())
                return false;
            m_engine = restored;
            return true;
        }

    private:
        std::mt19937 m_engine;
    };

    /**
     * @brief  [0, bound) 上的均匀整数（拒绝采样，无取模偏差）。
     * @param[in,out] rng   随机源；每次调用会推进其状态。
     * @param[in]     bound 上界（开区间）；`bound <= 1` 时恒返回 0。
     * @return 落在 [0, bound) 的均匀随机数。
     */
    inline std::uint32_t uniform_below(Rng &rng, std::uint32_t bound)
    {
        if (bound <= 1)
            return 0;
        const std::uint64_t range = std::uint64_t{1} << 32;
        const std::uint64_t limit = range - (range % bound);
        std::uint64_t x = 0;
        do
        {
            x = rng.next();
        } while (x >= limit);
        return static_cast<std::uint32_t>(x % bound);
    }
}

#endif  // INCLUDE_TKW_UTIL_RNG_HPP
