/**
 * @file   hero.cpp
 * @brief  `HeroQuery` 武将定义与技能查询的定义。
 * @details 承载武将解析、技能存在性、杀/闪转化来源与摸牌阶段张数；单表达式
 *          谓词 `ignores_trick_distance` 保留在 `hero.hpp` 内联。
 * @ingroup tkw_game_query
 */

#include "game/query/hero.hpp"

#include <string>
#include <vector>

namespace tkw
{
    namespace game
    {
        const hero::HeroDef *HeroQuery::hero_of(
            const ReadOnlyContext &ctx, const std::string &entity_id)
        {
            if (ctx.heroes == nullptr || ctx.entities == nullptr)
                return nullptr;
            const auto found = ctx.entities->find(entity_id);
            if (found.is_none())
                return nullptr;
            const std::string &id = found.unwrap()->get_hero();
            if (id.empty())
                return nullptr;
            const auto def = ctx.heroes->find(id);
            return def.is_some() ? def.unwrap() : nullptr;
        }

        bool HeroQuery::has_hero_skill(
            const ReadOnlyContext &ctx, const std::string &entity_id,
            hero::HeroSkill skill)
        {
            const hero::HeroDef *def = hero_of(ctx, entity_id);
            if (def == nullptr)
                return false;
            for (const auto s : def->skills)
                if (s == skill)
                    return true;
            return false;
        }

        bool HeroQuery::can_convert_card_to_sha(
            const ReadOnlyContext &ctx, const std::string &entity_id,
            const card::Card &c, const card::CardDef &def)
        {
            if (has_hero_skill(ctx, entity_id, hero::HeroSkill::WuSheng) &&
                StateQuery::is_red_suit(c.suit))
                return true;
            return has_hero_skill(ctx, entity_id, hero::HeroSkill::LongDan) &&
                   def.effect.is_some() &&
                   def.effect.unwrap().kind == card::CardEffectKind::Jink;
        }

        std::vector<card::Card> HeroQuery::sha_conversion_cards(
            const ReadOnlyContext &ctx, const std::string &entity_id)
        {
            std::vector<card::Card> out;
            for (const auto &c : ctx.cards->hand(entity_id))
            {
                const auto d = ctx.catalog->find(c.def_id);
                if (d.is_none())
                    continue;
                const card::CardDef &def = *d.unwrap();
                if (is_response_def(def, card::ResponseKind::Sha))
                    continue;
                if (can_convert_card_to_sha(ctx, entity_id, c, def))
                    out.push_back(c);
            }
            return out;
        }

        bool HeroQuery::can_convert_card_to_jink(
            const ReadOnlyContext &ctx, const std::string &entity_id,
            const card::Card &c, const card::CardDef &def)
        {
            if (has_hero_skill(ctx, entity_id, hero::HeroSkill::LongDan) &&
                def.effect.is_some() && is_sha_kind(def.effect.unwrap().kind))
                return true;
            return has_hero_skill(ctx, entity_id, hero::HeroSkill::QingGuo) &&
                   StateQuery::is_black_suit(c.suit);
        }

        std::vector<card::Card> HeroQuery::jink_conversion_cards(
            const ReadOnlyContext &ctx, const std::string &entity_id)
        {
            std::vector<card::Card> out;
            for (const auto &c : ctx.cards->hand(entity_id))
            {
                const auto d = ctx.catalog->find(c.def_id);
                if (d.is_none())
                    continue;
                const card::CardDef &def = *d.unwrap();
                if (is_response_def(def, card::ResponseKind::Jink))
                    continue;
                if (can_convert_card_to_jink(ctx, entity_id, c, def))
                    out.push_back(c);
            }
            return out;
        }

        int HeroQuery::draw_phase_count(
            const ReadOnlyContext &ctx, const std::string &entity_id)
        {
            int count = rules_of(ctx).draw_per_turn;
            if (has_hero_skill(ctx, entity_id, hero::HeroSkill::YingZi))
                ++count;
            return count;
        }
    }
}
