/**
 * @file hero.hpp
 * @brief 武将查询：按实体 id 解析其武将定义与技能。
 * @details 只读查询（query 层）：目录与实体都由调用方传入，本类不持有状态。
 *          无目录 / 实体无武将 / 目录未命中一律回落 `nullptr`/false，使无名
 *          座位在全流程中与「无技能」等价，不新增失败路径。
 * @ingroup tkw_game_query
 */

#ifndef INCLUDE_TKW_GAME_HERO_HPP
#define INCLUDE_TKW_GAME_HERO_HPP

#include <string>
#include <vector>

#include "card/card.hpp"
#include "card/def.hpp"
#include "game/core/context.hpp"
#include "game/core/effect.hpp"
#include "game/core/state.hpp"
#include "hero/catalog.hpp"
#include "hero/def.hpp"

namespace tkw
{
    namespace game
    {
        /**
         * @brief 武将只读查询（静态工具类）。
         * @details 纯函数集合：不持有上下文，`ctx` 由调用方传入；本类不改变任何状态。
         */
        class HeroQuery
        {
        public:
            /** @brief 静态工具类，不可实例化。 */
            HeroQuery() = delete;

            /**
             * @brief  返回实体所绑定武将的定义。
             * @param[in] ctx       只读上下文。
             * @param[in] entity_id 实体 id。
             * @return 实体所绑定的武将定义；无则 `nullptr`。
             * @retval nullptr `ctx.heroes` 空、`ctx.entities` 空、实体不存在、实体
             *         武将为空或目录未命中。
             * @note  武将身份创建后不可变，返回指针在目录生命周期内稳定。
             */
            static const hero::HeroDef *hero_of(
                const ReadOnlyContext &ctx, const std::string &entity_id);

            /**
             * @brief  实体是否拥有指定武将技能。
             * @param[in] ctx       只读上下文。
             * @param[in] entity_id 实体 id。
             * @param[in] skill     待查询的武将技能。
             * @return 拥有该技能时为 true。
             * @retval true 技能在该实体的武将 `skills` 中。
             * @retval false `hero_of` 未命中，或技能不在列表内。
             * @note  锁定技查询的唯一入口：行为消费点（如 `sha_limit`）经此判定，
             *        不直接读目录。
             */
            static bool has_hero_skill(
                const ReadOnlyContext &ctx, const std::string &entity_id,
                hero::HeroSkill skill);

            /**
             * @brief  该手牌能否被实体转化为虚拟「杀」（武圣红牌 / 龙胆闪）。
             * @param[in] ctx       只读上下文。
             * @param[in] entity_id 实体 id。
             * @param[in] c         手牌对象（武圣按花色判定）。
             * @param[in] def       该手牌的目录定义（龙胆按效果类别判定）。
             * @return 拥有对应转化技能且牌面满足来源条件 → true；否则 false。
             * @note  转化来源的唯一判定入口：主动枚举、响应存在性、响应候选与结算
             *        重识别共用，保证「哪些牌可当杀」各处口径一致。
             */
            static bool can_convert_card_to_sha(
                const ReadOnlyContext &ctx, const std::string &entity_id,
                const card::Card &c, const card::CardDef &def);

            /**
             * @brief  手牌中可作为虚拟杀打出的转化来源（手牌序，确定性）。
             * @param[in] ctx       只读上下文。
             * @param[in] entity_id 实体 id。
             * @return 满足 `can_convert_card_to_sha` 的手牌副本；目录未命中该牌的跳过。
             * @note  真杀已有普通出牌动作，跳过以避免同张牌重复产出；杀次数与目标
             *        合法性归校验层（主动侧 `validate_virtual_sha`）。
             */
            static std::vector<card::Card> sha_conversion_cards(
                const ReadOnlyContext &ctx, const std::string &entity_id);

            /**
             * @brief  该手牌能否被实体转化为虚拟「闪」（龙胆杀 / 倾国黑牌）。
             * @param[in] ctx       只读上下文。
             * @param[in] entity_id 实体 id。
             * @param[in] c         手牌对象（倾国按花色判定）。
             * @param[in] def       该手牌的目录定义（龙胆按效果类别判定）。
             * @return 拥有对应转化技能且牌面满足来源条件 → true；否则 false。
             * @note  转化来源的唯一判定入口：Jink 响应窗口的存在性、候选与消费共用，
             *        保证「哪些牌可当闪」各处口径一致。
             */
            static bool can_convert_card_to_jink(
                const ReadOnlyContext &ctx, const std::string &entity_id,
                const card::Card &c, const card::CardDef &def);

            /**
             * @brief  手牌中可作为虚拟闪打出的转化来源（手牌序，确定性）。
             * @param[in] ctx       只读上下文。
             * @param[in] entity_id 实体 id。
             * @return 满足 `can_convert_card_to_jink` 的手牌副本；目录未命中该牌的跳过。
             * @note  真闪已有普通响应，跳过以避免重复产出；闪无主动使用，仅响应窗口用。
             */
            static std::vector<card::Card> jink_conversion_cards(
                const ReadOnlyContext &ctx, const std::string &entity_id);

            /**
             * @brief  实体使用锦囊牌时是否无视距离限制（锁定技「奇才」）。
             * @param[in] ctx       只读上下文。
             * @param[in] entity_id 实体 id。
             * @return 拥有「奇才」时为 true；否则 false。
             * @retval true 拥有锁定技「奇才」。
             * @retval false 无目录 / 实体无武将 / 无奇才。
             * @note  锦囊距离判定的唯一判定入口：主动锦囊（顺手牵羊）的目标枚举与
             *        预校验、延时锦囊（兵粮寸断）的置入范围三处共用，保证口径一致。
             *        仅锦囊成立；「杀」的攻击范围仍走距离查询，不受本函数影响。
             */
            static bool ignores_trick_distance(
                const ReadOnlyContext &ctx, const std::string &entity_id);

            /**
             * @brief  摸牌阶段摸牌张数：`rules.draw_per_turn`，锁定技「英姿」再 +1。
             * @param[in] ctx       只读上下文。
             * @param[in] entity_id 实体 id。
             * @return 该实体本回合摸牌阶段的摸牌张数。
             * @retval +1 拥有锁定技「英姿」时在规则基数上再加一。
             * @note  摸牌阶段张数的唯一采样点；兵粮寸断跳过整个摸牌阶段时不经本
             *        函数，故英姿不会越过跳过语义。
             */
            static int draw_phase_count(
                const ReadOnlyContext &ctx, const std::string &entity_id);
        };

        inline const hero::HeroDef *HeroQuery::hero_of(
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

        inline bool HeroQuery::has_hero_skill(
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

        inline bool HeroQuery::can_convert_card_to_sha(
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

        inline std::vector<card::Card> HeroQuery::sha_conversion_cards(
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

        inline bool HeroQuery::can_convert_card_to_jink(
            const ReadOnlyContext &ctx, const std::string &entity_id,
            const card::Card &c, const card::CardDef &def)
        {
            if (has_hero_skill(ctx, entity_id, hero::HeroSkill::LongDan) &&
                def.effect.is_some() && is_sha_kind(def.effect.unwrap().kind))
                return true;
            return has_hero_skill(ctx, entity_id, hero::HeroSkill::QingGuo) &&
                   StateQuery::is_black_suit(c.suit);
        }

        inline std::vector<card::Card> HeroQuery::jink_conversion_cards(
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

        inline bool HeroQuery::ignores_trick_distance(
            const ReadOnlyContext &ctx, const std::string &entity_id)
        {
            return has_hero_skill(ctx, entity_id, hero::HeroSkill::QiCai);
        }

        inline int HeroQuery::draw_phase_count(
            const ReadOnlyContext &ctx, const std::string &entity_id)
        {
            int count = rules_of(ctx).draw_per_turn;
            if (has_hero_skill(ctx, entity_id, hero::HeroSkill::YingZi))
                ++count;
            return count;
        }
    }
}

#endif  // INCLUDE_TKW_GAME_HERO_HPP
