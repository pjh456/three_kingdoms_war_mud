/**
 * @file state.hpp
 * @brief 对局状态的基础操作：扣血/回血/摸牌。
 * @details 不含濒死与死亡判定，那些归 combat 结算。这些是「状态层」原语：
 *          只改实体状态与牌堆，不发布流程事件（濒死/死亡），但会发布卡牌域
 *          事件（如摸牌）供日志/回放消费。
 * @warning 改对局状态只能经本文件或 resolve 层；query/AI 目录不得调用其中的
 *          非 const 接口。
 * @ingroup tkw_game_core
 */

#ifndef INCLUDE_TKW_GAME_STATE_HPP
#define INCLUDE_TKW_GAME_STATE_HPP

#include <algorithm>
#include <string>
#include <utility>

#include "card/def.hpp"
#include "card/manager.hpp"
#include "entity/manager.hpp"
#include "game/core/card_event.hpp"
#include "game/core/context.hpp"
#include "game/core/decision.hpp"
#include "util/types.hpp"

namespace tkw
{
    namespace game
    {
        /**
         * @brief 只读状态/卡牌谓词（静态工具类）。
         * @details 纯函数集合：不持有上下文，`ctx` 由调用方传入；本类不改变任何状态。
         */
        class StateQuery
        {
        public:
            /** @brief 静态工具类，不可实例化。 */
            StateQuery() = delete;

            /**
             * @brief  花色是否为黑（♠/♣）。
             * @param[in] s 花色。
             * @return 黑桃或梅花时为 true。
             */
            static bool is_black_suit(card::Suit s);

            /**
             * @brief  花色是否为红（♥/♦）。
             * @param[in] s 花色。
             * @return 红桃或方块时为 true。
             */
            static bool is_red_suit(card::Suit s);

            /**
             * @brief  判定牌是否满足触发条件（条件来自数据）。
             * @param[in] t 判定触发条件。
             * @param[in] c 揭示的判定牌。
             * @return 判定牌满足该条件时为 true；未知条件为 false。
             */
            static bool judge_triggered(card::JudgeTrigger t, const card::Card &c);

            /**
             * @brief  判定结果动作。
             * @param[in] j 判定效果（触发条件与成功/失败动作）。
             * @param[in] c 揭示的判定牌。
             * @return 满足触发条件时取 `j.success`，否则取 `j.failure`。
             */
            static card::JudgeAction judge_result(
                const card::JudgeEffect &j, const card::Card &c);

            /**
             * @brief  目录查找。
             * @param[in] ctx    只读上下文。
             * @param[in] def_id 卡牌定义 id。
             * @return 命中的定义指针；未命中或目录未绑定时为 `nullptr`。
             * @note  返回指针在目录生命周期内稳定。
             */
            static const card::CardDef *def_of(
                const ReadOnlyContext &ctx, const std::string &def_id);

            /**
             * @brief  单张牌是否满足谓词。
             * @tparam Accept 谓词类型：接受 `const card::CardDef &`、返回 bool。
             * @param[in] ctx    只读上下文。
             * @param[in] c      待判定的牌。
             * @param[in] accept 对牌面定义求值的谓词。
             * @return 目录命中且谓词为 true 时为 true；目录缺失或未命中为 false。
             */
            template <class Accept>
            static bool hand_card_matching(
                const ReadOnlyContext &ctx, const card::Card &c, Accept accept);

            /**
             * @brief  手牌中是否存在满足谓词者（只读扫描）。
             * @tparam Accept 谓词类型：接受 `const card::CardDef &`、返回 bool。
             * @param[in] ctx    只读上下文。
             * @param[in] owner  手牌持有者 id。
             * @param[in] accept 对牌面定义求值的谓词。
             * @return 存在满足谓词的手牌时为 true。
             * @post  本接口不改变任何状态。
             */
            template <class Accept>
            static bool any_hand_card_matching(
                const ReadOnlyContext &ctx, const std::string &owner, Accept accept);

            /**
             * @brief  手牌中首张满足谓词者（只读副本）。
             * @tparam Accept 谓词类型：接受 `const card::CardDef &`、返回 bool。
             * @param[in] ctx    只读上下文。
             * @param[in] owner  手牌持有者 id。
             * @param[in] accept 对牌面定义求值的谓词。
             * @return 首张匹配的手牌副本；无匹配时为 `None`。
             * @retval Some 按手牌序命中的首张牌。
             * @retval None 无匹配（含目录缺失或未命中）。
             * @post  本接口不改变任何状态。
             */
            template <class Accept>
            static Option<card::Card> find_hand_card_matching(
                const ReadOnlyContext &ctx, const std::string &owner, Accept accept);

            /**
             * @brief  目标是否处于连环状态。
             * @param[in] ctx 只读上下文。
             * @param[in] id  实体 id。
             * @return 实体存在且已横置时为 true；实体不存在时为 false。
             */
            static bool is_chained(const ReadOnlyContext &ctx, const std::string &id);
        };

        /**
         * @brief 对局状态写操作（操作类）。
         * @details 持对局上下文（引用、非拥有），方法不再逐个传 `ctx`；本类只改实体
         *          状态与牌堆并发布卡牌域事件，不含濒死/死亡判定。
         * @warning 不拥有 `m_ctx`：被其引用的容器/目录须比本对象存活更久；不得跨局复用。
         */
        class StateOps
        {
        public:
            /**
             * @brief  绑定对局上下文。
             * @param[in,out] ctx 对局上下文；本对象只持引用。
             */
            explicit StateOps(GameContext &ctx) : m_ctx(ctx) {}

            /**
             * @brief  回血（按上限钳制）。
             * @param[in] target 回复对象 id。
             * @param[in] amount 回复量；实体不存在时安全 no-op。
             * @post  实际回复量受实体体力上限钳制。
             */
            void apply_heal(const std::string &target, int amount);

            /**
             * @brief  设置目标的连环状态（横置/重置）。
             * @param[in] id      目标实体 id。
             * @param[in] chained 目标状态。
             * @note  空 id 或实体不存在时安全 no-op；不发事件，调用方负责结算语义。
             */
            void set_chained(const std::string &id, bool chained);

            /**
             * @brief  失去装备区一张牌后的触发结算。
             * @details 带「白银狮子」能力者回复 1 点体力。
             * @param[in] owner 失去装备的实体 id（回复对象，非取牌者）。
             * @param[in] card  离开装备区的牌。
             * @note  仅装备区失去触发；手牌/判定区的同名卡离场不触发（调用方保证
             *        来源为 `Equip`）。满体力时 heal 钳制为 0，无副作用。
             */
            void apply_equip_lost(const std::string &owner, const card::Card &card);

            /**
             * @brief  消费酒对本回合下一张「杀」的伤害加成。
             * @details 归属匹配才生效且只生效一次。
             * @param[in] attacker 本次使用「杀」的玩家 id。
             * @return 应叠加的伤害基数（+1）；无待生效加成或归属不符时为 0。
             * @note  只清归属不碰「本回合已用酒」标记：该标记由回合入口清零。
             *        消费点放在「使用杀」的入口，响应/打出的杀不消费也不享受。
             */
            int consume_jiu_sha_bonus(const std::string &attacker);

            /**
             * @brief  从摸牌堆取一张；牌堆空且弃牌堆非空时经 rng 洗回后重试。
             * @return 摸到的牌；无牌可摸时为 `None`。
             * @retval Some 摸牌堆顶的牌（必要时先经 `m_ctx.rng` 洗回弃牌堆）。
             * @retval None 摸牌堆与弃牌堆皆空（或无 rng 且摸牌堆空）。
             * @note  随机源为 null 时牌堆空则直接 `None`。
             */
            Option<card::Card> draw_with_refill();

            /**
             * @brief  摸 count 张进手牌；牌堆与弃牌堆皆空即停。
             * @param[in] player 摸牌入手的实体 id。
             * @param[in] count  请求摸牌张数。
             * @param[in] kind   摸牌来源语义，透传到摸牌事件供日志标签区分。
             * @return 实际摸到的张数（提前无牌可摸时小于 `count`）。
             * @post  每张入手的牌发布一次摸牌事件。
             */
            int apply_draw(
                const std::string &player, int count,
                DrawKind kind = DrawKind::Normal);

            /**
             * @brief  从某实体的任一区域移除指定牌。
             * @param[in]  entity_id   实体 id。
             * @param[in]  instance_id 待移除的牌实例 id。
             * @param[out] out         被移除的牌；失败时不被修改。
             * @param[out] from_zone   来源区域；非空时写入，可为 `nullptr`。
             * @return 移除成功时为 true；该牌不在任一区域时为 false。
             * @post  失败时 `out` 与牌区均不被污染。
             */
            bool remove_card_from_zones(
                const std::string &entity_id, const std::string &instance_id,
                card::Card &out, Zone *from_zone = nullptr);

            /**
             * @brief  弃置一张牌并发布弃置事件。
             * @param[in] owner 弃置归属实体 id（可空 = 判定/无主）。
             * @param[in] c     待弃置的牌。
             * @param[in] kind  进弃牌堆的来源语义（供展示标签区分）。
             * @note  按值拷贝入弃牌堆，事件读原牌。
             */
            void discard_and_emit(
                const std::string &owner, const card::Card &c,
                DiscardKind kind = DiscardKind::Normal);

            /**
             * @brief  从手牌移除指定牌并无条件弃置，发布弃置事件。
             * @param[in] owner       手牌持有者 id。
             * @param[in] instance_id 待移除的手牌实例。
             * @return 被弃置的牌；牌不在手牌时为 `None`。
             * @retval Some 已从手牌移除并进入弃牌堆的牌。
             * @retval None 该牌不在 `owner` 手牌（无副作用）。
             * @note  弃置按值拷贝、事件读原牌后再整体移动返回，返回值保持完整。
             */
            Option<card::Card> remove_and_discard(
                const std::string &owner, const std::string &instance_id);

            /**
             * @brief  从任一区域移除指定牌并无条件弃置，发布弃置事件。
             * @param[in] owner       牌所属实体 id。
             * @param[in] instance_id 待移除的牌实例（按手牌 → 装备 → 判定顺序查找）。
             * @return 被弃置的牌；牌不在 `owner` 任一区域时为 `None`。
             * @retval Some 已移除并进入弃牌堆的牌；来源为装备区时附带触发失去结算。
             * @retval None 该牌不在 `owner` 任一区域（无副作用）。
             * @note  弃置按值拷贝、事件读原牌后再整体移动返回，返回值保持完整。
             */
            Option<card::Card> remove_any_and_discard(
                const std::string &owner, const std::string &instance_id);

            /**
             * @brief  从手牌移除指定牌并按谓词校验；合法则弃置并发布弃置事件。
             * @tparam Accept 谓词类型：接受 `const card::CardDef &` 与
             *         `const card::Card &`、返回 bool。
             * @param[in] owner       手牌持有者 id。
             * @param[in] instance_id 待消费的手牌实例。
             * @param[in] accept      校验所选牌的谓词。
             * @param[in] kind        进弃牌堆的来源语义（响应/判定/真实弃置）。
             * @return 被消费的牌；牌不在手牌或校验失败时为 `None`。
             * @retval Some 校验通过并进入弃牌堆的牌。
             * @retval None 牌不在手牌，或目录缺失/谓词不匹配（非法选择已退回手牌）。
             * @note  弃置按值拷贝、事件读原牌后再整体移动返回，返回值保持完整。
             *        谓词接收牌面：花色在 Card 上，转化类判定（黑牌当闪等）需要。
             */
            template <class Accept>
            Option<card::Card> consume_hand_card_matching(
                const std::string &owner, const std::string &instance_id,
                Accept accept, DiscardKind kind = DiscardKind::Normal);

            /**
             * @brief  判定：从摸牌堆顶揭示一张（牌堆空则弃牌堆洗回）。
             * @return 揭示的判定牌；无法判定时为 `None`。
             * @retval Some 摸牌堆顶的牌。
             * @retval None 摸牌堆与弃牌堆皆空。
             * @note  洗回口径与摸牌一致：同走 `draw_with_refill`。
             */
            Option<card::Card> perform_judgement();

            /**
             * @brief  落地目标区域选牌：明置牌直接返回，隐藏手牌经 rng 均匀暗抽。
             * @param[in] target 被选牌的目标实体 id。
             * @param[in] pick   决策源回传的区域/槽位/明置牌；隐藏手牌 card == None。
             * @return 选中的真实手牌/明置牌；无可选牌时为 `None`。
             * @retval Some 决策源明示的牌，或按槽位从当前手牌快照抽出的牌。
             * @retval None 目标手牌为空且无可选明置牌。
             * @note  每次调用按当前手牌快照重算，不缓存槽位（寒冰剑连取时手牌持续
             *        收缩）。`m_ctx.rng` 为空时回落候选槽位（无随机源测试）；手牌仅
             *        1 张时 `uniform_below` 因 bound <= 1 不消费随机流。
             */
            Option<card::Card> resolve_target_pick(
                const std::string &target, const TargetPick &pick);

        private:
            GameContext &m_ctx; /**< 对局上下文（引用，非拥有）。 */
        };

        inline bool StateQuery::is_black_suit(card::Suit s)
        {
            return s == card::Suit::Spade || s == card::Suit::Club;
        }

        inline bool StateQuery::is_red_suit(card::Suit s)
        {
            return s == card::Suit::Heart || s == card::Suit::Diamond;
        }

        inline bool StateQuery::judge_triggered(
            card::JudgeTrigger t, const card::Card &c)
        {
            switch (t)
            {
            case card::JudgeTrigger::Red:
                return is_red_suit(c.suit);
            case card::JudgeTrigger::Black:
                return is_black_suit(c.suit);
            case card::JudgeTrigger::Heart:
                return c.suit == card::Suit::Heart;
            case card::JudgeTrigger::NotHeart:
                return c.suit != card::Suit::Heart;
            case card::JudgeTrigger::Spade2to9:
                return c.suit == card::Suit::Spade && c.number >= 2 && c.number <= 9;
            case card::JudgeTrigger::NotClub:
                return c.suit != card::Suit::Club;
            }
            return false;
        }

        inline card::JudgeAction StateQuery::judge_result(
            const card::JudgeEffect &j, const card::Card &c)
        {
            return judge_triggered(j.trigger, c) ? j.success : j.failure;
        }

        inline const card::CardDef *StateQuery::def_of(
            const ReadOnlyContext &ctx, const std::string &def_id)
        {
            if (!ctx.catalog)
                return nullptr;
            const auto def = ctx.catalog->find(def_id);
            return def.is_some() ? def.unwrap() : nullptr;
        }

        template <class Accept>
        inline bool StateQuery::hand_card_matching(
            const ReadOnlyContext &ctx, const card::Card &c, Accept accept)
        {
            const card::CardDef *def = def_of(ctx, c.def_id);
            return def != nullptr && accept(*def);
        }

        template <class Accept>
        inline bool StateQuery::any_hand_card_matching(
            const ReadOnlyContext &ctx, const std::string &owner, Accept accept)
        {
            for (const auto &c : ctx.cards->hand(owner))
                if (hand_card_matching(ctx, c, accept))
                    return true;
            return false;
        }

        template <class Accept>
        inline Option<card::Card> StateQuery::find_hand_card_matching(
            const ReadOnlyContext &ctx, const std::string &owner, Accept accept)
        {
            for (const auto &c : ctx.cards->hand(owner))
                if (hand_card_matching(ctx, c, accept))
                    return Option<card::Card>::Some(c);
            return Option<card::Card>::None();
        }

        inline bool StateQuery::is_chained(
            const ReadOnlyContext &ctx, const std::string &id)
        {
            const auto e = ctx.entities->find(id);
            return e.is_some() && e.unwrap()->get_chained();
        }

        inline void StateOps::apply_heal(const std::string &target, int amount)
        {
            const auto e = m_ctx.entities->find(target);
            if (e.is_some())
                e.unwrap()->heal(amount);
        }

        inline void StateOps::set_chained(const std::string &id, bool chained)
        {
            const auto e = m_ctx.entities->find(id);
            if (e.is_some())
                e.unwrap()->set_chained(chained);
        }

        inline void StateOps::apply_equip_lost(
            const std::string &owner, const card::Card &card)
        {
            const card::CardDef *def = StateQuery::def_of(m_ctx, card.def_id);
            if (def == nullptr)
                return;
            if (std::find(def->abilities.begin(), def->abilities.end(),
                          card::Ability::SilverLion) == def->abilities.end())
                return;
            apply_heal(owner, 1);
        }

        inline int StateOps::consume_jiu_sha_bonus(const std::string &attacker)
        {
            if (m_ctx.jiu_damage_owner != attacker)
                return 0;
            m_ctx.jiu_damage_owner.clear();
            return 1;
        }

        inline Option<card::Card> StateOps::draw_with_refill()
        {
            if (m_ctx.cards->draw_size() == 0)
            {
                if (m_ctx.cards->discard_size() == 0)
                    return Option<card::Card>::None();
                if (m_ctx.rng)
                    m_ctx.cards->refill_draw(*m_ctx.rng);
            }
            return m_ctx.cards->draw();
        }

        inline int StateOps::apply_draw(
            const std::string &player, int count, DrawKind kind)
        {
            int drew = 0;
            for (int i = 0; i < count; ++i)
            {
                auto c = draw_with_refill();
                if (c.is_none())
                    break;
                card::Card card = std::move(c).unwrap();
                m_ctx.cards->add_to_hand(player, card);
                emit_card_drawn(m_ctx, player, card, kind);
                ++drew;
            }
            return drew;
        }

        inline bool StateOps::remove_card_from_zones(
            const std::string &entity_id, const std::string &instance_id,
            card::Card &out, Zone *from_zone)
        {
            auto c = m_ctx.cards->remove_from_any(entity_id, instance_id, from_zone);
            if (c.is_none())
                return false;
            out = std::move(c).unwrap();
            return true;
        }

        inline void StateOps::discard_and_emit(
            const std::string &owner, const card::Card &c, DiscardKind kind)
        {
            m_ctx.cards->discard(c);
            emit_card_discarded(m_ctx, owner, c, kind);
        }

        inline Option<card::Card> StateOps::remove_and_discard(
            const std::string &owner, const std::string &instance_id)
        {
            auto removed = m_ctx.cards->remove_from_hand(owner, instance_id);
            if (removed.is_none())
                return Option<card::Card>::None();
            card::Card card = std::move(removed).unwrap();
            discard_and_emit(owner, card);
            return Option<card::Card>::Some(std::move(card));
        }

        inline Option<card::Card> StateOps::remove_any_and_discard(
            const std::string &owner, const std::string &instance_id)
        {
            card::Card card;
            Zone from = Zone::Limbo;
            if (!remove_card_from_zones(owner, instance_id, card, &from))
                return Option<card::Card>::None();
            discard_and_emit(owner, card);

            // 装备区失去触发（过河/寒冰选装备区时）：弃置后结算回血
            if (from == Zone::Equip)
                apply_equip_lost(owner, card);
            return Option<card::Card>::Some(std::move(card));
        }

        template <class Accept>
        inline Option<card::Card> StateOps::consume_hand_card_matching(
            const std::string &owner, const std::string &instance_id,
            Accept accept, DiscardKind kind)
        {
            auto removed = m_ctx.cards->remove_from_hand(owner, instance_id);
            if (removed.is_none())
                return Option<card::Card>::None();
            card::Card card = std::move(removed).unwrap();

            const auto def = m_ctx.catalog->find(card.def_id);
            if (def.is_none() || !accept(*def.unwrap(), card))
            {
                m_ctx.cards->add_to_hand(owner, std::move(card));  // 非法选择退回
                return Option<card::Card>::None();
            }

            discard_and_emit(owner, card, kind);
            return Option<card::Card>::Some(std::move(card));
        }

        inline Option<card::Card> StateOps::perform_judgement()
        {
            return draw_with_refill();
        }

        inline Option<card::Card> StateOps::resolve_target_pick(
            const std::string &target, const TargetPick &pick)
        {
            if (pick.card.is_some())
                return pick.card;

            const auto hand = m_ctx.cards->hand(target);
            if (hand.empty())
                return Option<card::Card>::None();

            std::size_t i = pick.index < hand.size() ? pick.index : 0;
            if (m_ctx.rng)
                i = uniform_below(*m_ctx.rng, static_cast<std::uint32_t>(hand.size()));
            return Option<card::Card>::Some(hand[i]);
        }
    }
}

#endif  // INCLUDE_TKW_GAME_STATE_HPP
