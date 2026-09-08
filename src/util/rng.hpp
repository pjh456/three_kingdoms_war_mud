/**
 * @file rng.hpp
 * @brief 随机源抽象：把引擎从 std::mt19937 解绑，便于注入确定性/可记录的序列。
 * @note 接口暴露 next()（32 位均匀）与 save_state/load_state（存档续档用）；
 *       洗牌等消费方用 uniform_below 做拒绝采样，保证无取模偏差。
 */

#ifndef INCLUDE_TKW_UTIL_RNG_HPP
#define INCLUDE_TKW_UTIL_RNG_HPP

#include <cstdint>
#include <random>
#include <sstream>
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
        std::string data;

        bool operator==(const RngState &) const = default;
    };

    /** @brief 随机源接口（对局随机性的唯一入口）。 */
    struct Rng
    {
        virtual ~Rng() = default;

        /** @brief 下一个 32 位均匀随机数。 */
        virtual std::uint32_t next() = 0;

        /** @brief 导出当前状态（存档用）。 */
        virtual RngState save_state() const = 0;

        /** @brief 恢复状态；data 非法时返回 false（引擎保持原状）。 */
        virtual bool load_state(const RngState &state) = 0;
    };

    /** @brief 种子化 mt19937 实现（生产与测试默认）。 */
    class SeededRng : public Rng
    {
    public:
        explicit SeededRng(std::uint32_t seed) : engine_(seed) {}

        std::uint32_t next() override { return engine_(); }

        RngState save_state() const override
        {
            std::ostringstream os;
            os << engine_;
            return RngState{os.str()};
        }

        bool load_state(const RngState &state) override
        {
            std::istringstream is(state.data);
            std::mt19937 restored;
            is >> restored;
            if (is.fail())
                return false;
            engine_ = restored;
            return true;
        }

    private:
        std::mt19937 engine_;
    };

    /** @brief [0, bound) 上的均匀整数（拒绝采样，无取模偏差）。 */
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
