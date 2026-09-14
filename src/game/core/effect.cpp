/**
 * @file   effect.cpp
 * @brief  响应牌判定与出牌路径分类的定义。
 * @details 实现 `effect.hpp` 中仅有的两个多分支非 `constexpr` 函数；属性表与
 *          单表达式谓词保留在头内。
 * @ingroup tkw_game_core
 */

#include "game/core/effect.hpp"

namespace tkw
{
    namespace game
    {
        bool is_response_def(const card::CardDef &def, card::ResponseKind kind)
        {
            if (def.effect.is_none())
                return false;
            const auto k = def.effect.unwrap().kind;
            switch (kind)
            {
            case card::ResponseKind::Sha:
                return is_sha_kind(k);
            case card::ResponseKind::Jink:
                return k == card::CardEffectKind::Jink;
            }
            return false;
        }

        PlayClass classify_action(const card::CardDef &def)
        {
            if (def.type == card::CardType::Equipment)
                return PlayClass::Equipment;
            if (is_delayed_trick(def))
                return PlayClass::DelayedTrick;
            if (def.effect.is_some())
                return PlayClass::Active;
            return PlayClass::None;
        }
    }
}
