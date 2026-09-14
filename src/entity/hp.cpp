/**
 * @file   hp.cpp
 * @brief  血条模型 `Hp` 状态修改入口的定义。
 * @ingroup tkw_entity
 */

#include "entity/hp.hpp"

#include <algorithm>

namespace tkw
{
    namespace entity
    {
        Hp Hp::make(int initial)
        {
            Hp hp;
            hp.m_cur = initial;
            hp.m_max = initial;
            return hp;
        }

        bool Hp::set_cur(int val)
        {
            if (val > m_max)
                return false;
            if (val == m_cur)
                return true;
            const int old = m_cur;
            m_cur = val;
            if (m_on_change)
                m_on_change(old, m_cur, m_max);
            return true;
        }

        bool Hp::set_max(int val)
        {
            if (val < 0)
                return false;
            if (val == m_max)
                return true;
            m_max = val;
            const int old_cur = m_cur;
            if (m_cur > m_max)
                m_cur = m_max;
            if (m_cur != old_cur && m_on_change)
                m_on_change(old_cur, m_cur, m_max);
            return true;
        }

        int Hp::add(int det)
        {
            const int old = m_cur;
            set_cur(std::min(m_cur + det, m_max));
            return m_cur - old;
        }

        int Hp::sub(int det)
        {
            const int old = m_cur;
            set_cur(m_cur - det);
            return old - m_cur;
        }
    }
}
