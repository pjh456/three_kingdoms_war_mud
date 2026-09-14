/**
 * @file   rng.cpp
 * @brief  随机源抽象与种子化 `mt19937` 实现的定义。
 * @ingroup tkw_util
 */

#include "util/rng.hpp"

#include <cstdint>
#include <ios>
#include <random>
#include <sstream>
#include <stdexcept>

namespace tkw
{
    SeededRng::SeededRng(std::uint32_t seed) : m_engine(seed) {}

    std::uint32_t SeededRng::next()
    {
        return m_engine();
    }

    RngState SeededRng::save_state() const
    {
        std::ostringstream os;
        os << m_engine;
        return RngState{os.str()};
    }

    bool SeededRng::load_state(const RngState &state)
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

    std::uint32_t uniform_below(Rng &rng, std::uint32_t bound)
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
