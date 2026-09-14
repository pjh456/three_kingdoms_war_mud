/**
 * @file   decider.hpp
 * @brief  统一决策接缝：把 DecisionSource 的分散回调收敛成单个 decide(DecisionRequest)。
 * @details 并附带只读观察与候选枚举。状态机只需实现 `Decider::decide`；
 *          `RequestDecisionSource` 负责从 `ReadOnlyContext` 抽取观察/候选并适配回引擎的
 *          `DecisionSource`。请求只含只读数据（观察 + 候选 + 卡牌目录），故决策是
 *          纯函数、可回放、可单测。本层为只读 AI，不修改任何对局状态。
 * @ingroup tkw_game_ai
 */

#ifndef INCLUDE_TKW_GAME_DECIDER_HPP
#define INCLUDE_TKW_GAME_DECIDER_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "card/card.hpp"
#include "card/catalog.hpp"
#include "card/def.hpp"
#include "game/ai/legal.hpp"
#include "game/ai/view.hpp"
#include "game/core/decision.hpp"
#include "game/core/effect.hpp"
#include "game/core/state.hpp"
#include "game/query/hero.hpp"
#include "util/types.hpp"

namespace tkw
{
    namespace game
    {
        namespace ai
        {
            /** @brief 决策点类别（与 DecisionSource 的回调一一对应）。 */
            enum class DecisionKind : std::uint8_t
            {
                Play,         /**< 出牌阶段选动作 */
                Response,     /**< 响应窗口（杀/闪） */
                Peach,        /**< 濒死救场 */
                Counter,      /**< 无懈窗口 */
                Trigger,      /**< 可选装备能力是否发动 */
                PickCard,     /**< 从目标区域选牌 */
                PickRevealed, /**< 从亮出的牌中选牌 */
                Discard,      /**< 弃牌阶段/能力代价 */
            };

            /** @brief 一次决策请求：类别 + 决策者 + 只读观察 + 候选。 */
            struct DecisionRequest
            {
                DecisionKind kind = DecisionKind::Play; /**< 决策点类别 */
                std::string actor; /**< 决策者 id */
                AiView view;       /**< 决策者视角观察 */
                const card::CardDefCatalog *catalog = nullptr; /**< 只读卡牌目录 */

                // Play
                TurnContext turn; /**< Play：当前回合上下文（杀次数上限等） */

                // Play / Response
                std::vector<LegalAction> legal; /**< Play 合法出牌动作；Response 两张当杀 pair 候选（丈八） */

                // Response / Peach / Counter / PickCard / PickRevealed / Discard
                // Response 单牌候选 = 真响应牌 + 单张转化（武圣红牌/龙胆闪当杀、
                // 龙胆杀当闪）；
                // PickCard 的对手手牌候选为无身份占位槽（def_id/instance_id 空，
                // zone_labels 仍标 Hand）；其余类别均为决策者可见牌或公开亮牌。
                std::vector<card::Card> options; /**< 各类别的候选牌（口径见上方注释） */

                // PickRevealed
                RevealSource reveal_source = RevealSource::Wugu; /**< 亮牌来源结算 */

                // Response
                card::ResponseKind response_kind = card::ResponseKind::Sha; /**< Response：需要的响应牌类别 */
                std::string response_source; /**< Response：来源牌 def id（空 = 未知） */
                std::string response_user;   /**< Response：来源使用者 id */
                int response_damage = 0;     /**< Response：不响应伤害量（0 = 不适用） */

                // Peach
                std::string dying; /**< Peach：濒死者 id */

                // Counter
                std::string counter_user;              /**< 锦囊使用者（延时判定窗口 = 空串哨兵） */
                std::vector<std::string> counter_targets; /**< 锦囊目标集合（判定窗口 = 被判定玩家一人） */
                std::string counter_trick;             /**< 被无懈的锦囊 def id（空 = 未知） */
                int counter_played = 0; /**< 本窗已打出的无懈张数（公开事实，0 起） */

                // PickCard
                std::string target; /**< PickCard：被选牌的目标 id */
                std::vector<card::Zone> zone_labels; /**< PickCard：与 options 等长的来源分区标签；其余类别为空 */

                // Trigger
                card::Ability ability = card::Ability::NoShaLimit; /**< Trigger：装备能力（hero_trigger=false 时有效） */
                bool hero_trigger = false; /**< Trigger：true=武将触发技，false=装备能力 */
                hero::HeroSkill hero_skill =
                    hero::HeroSkill::PaoXiao; /**< Trigger：武将触发技技能 */
                std::string trigger_cause; /**< Trigger：触发来源（反馈=伤害来源；空=无来源） */

                // Discard
                int count = 0; /**< Discard：需弃牌数 */
                DiscardReason discard_reason = DiscardReason::TurnLimit; /**< Discard：弃牌原因 */
            };

            /** @brief 决策结果（按类别取用相应字段）。 */
            struct DecisionChoice
            {
                bool accepted = false; /**< Trigger：是否发动 */
                Option<std::string> instance_id = Option<std::string>::None(); /**< Play/Response/Peach/Counter：选中的牌实例 id；`None` = 放弃 */
                Option<std::size_t> option_index =
                    Option<std::size_t>::None(); /**< PickCard/PickRevealed：选中的候选下标 */
                std::vector<std::string> targets;  /**< Play：目标 */
                std::vector<std::string> discards; /**< Discard：要弃的牌 */
                std::string second_instance_id;   /**< Play/Response：第二张手牌（丈八蛇矛两张当杀；空 = 普通） */
                bool recast = false; /**< Play：重铸动作（弃置此牌并摸一张，targets 为空） */
                bool converted_sha = false; /**< Play：单张转化当杀（武圣红牌 / 龙胆闪；来源由引擎按武将判定） */
            };

            /**
             * @brief  状态机接口：实现单个 decide 即可接入引擎。
             * @warning 实现必须是只读的：不得修改对局状态或缓存请求内指针。
             */
            class Decider
            {
            public:
                virtual ~Decider() = default; /**< 多态析构；派生决策源经基类指针释放。 */

                /**
                 * @brief  对一次决策请求给出选择。
                 * @param[in] request 类别 + 决策者 + 只读观察 + 候选；生命周期覆盖本次调用。
                 * @return 按 `request.kind` 取用相应字段的决策结果。
                 * @pre   `request` 生命周期覆盖本次调用。
                 * @post  本接口不改变任何对局状态；请求与观察只读。
                 * @note  实现应确定性、可回放；返回空 `DecisionChoice` 表示放弃。
                 */
                virtual DecisionChoice decide(const DecisionRequest &request) = 0;
            };

            /**
             * @class RequestDecisionSource
             * @brief 把 Decider 适配回引擎的 DecisionSource：负责从 GameContext
             *        抽取观察与候选，构造 DecisionRequest，再翻译 DecisionChoice。
             * @note  只读：全部回调只读取容器并构造副本，不修改对局状态。
             */
            class RequestDecisionSource : public DecisionSource
            {
            public:
                /**
                 * @brief  以指定决策器构造适配器。
                 * @param[in] decider 决策实现；生命周期须覆盖本对象。
                 */
                explicit RequestDecisionSource(Decider &decider) : m_decider(&decider) {}

                /**
                 * @brief  响应窗口适配：枚举真响应牌与单张/两张转化候选并翻译选择。
                 * @param[in] ctx    只读容器视图。
                 * @param[in] entity 被询问的实体 id。
                 * @param[in] kind   需要的响应牌类别。
                 * @param[in] prompt 来源牌/使用者/伤害量（只读事实）。
                 * @return 要打出的响应动作；`None` = 不响应。
                 * @retval Some 引擎校验并负责消费所选手牌。
                 * @retval None 放弃响应。
                 * @post 本接口不改变任何状态。
                 */
                Option<PlayAction> play_response(
                    const ReadOnlyContext &ctx, const std::string &entity,
                    card::ResponseKind kind, const ResponsePrompt &prompt) override
                {
                    DecisionRequest req = base_request(ctx, entity);
                    req.kind = DecisionKind::Response;
                    req.response_kind = kind;
                    req.response_source = prompt.source_def_id;
                    req.response_user = prompt.source_user;
                    req.response_damage = prompt.damage;
                    for (const auto &c : ctx.cards->hand(entity))
                        if (is_response_card(ctx, c, kind))
                            req.options.push_back(c);
                    // 单张转化当杀（武圣红牌 / 龙胆闪）：杀响应窗口并入单牌
                    // 候选（真响应牌优先在前，同张真杀不重复；转化合法性由
                    // 引擎按武将 + 牌面识别）
                    if (kind == card::ResponseKind::Sha)
                        for (const auto &c : HeroQuery::sha_conversion_cards(ctx, entity))
                            req.options.push_back(c);
                    // 单张转化当闪（龙胆杀）：闪响应窗口并入单牌候选
                    if (kind == card::ResponseKind::Jink)
                        for (const auto &c : HeroQuery::jink_conversion_cards(ctx, entity))
                            req.options.push_back(c);
                    // 杀响应窗口：携带两张当杀 pair 候选（与主动侧同一枚举口径）
                    if (kind == card::ResponseKind::Sha)
                        for (const auto &[first, second] :
                             two_cards_as_sha_pairs(ctx, entity))
                            req.legal.push_back(LegalAction{first, {}, second.instance_id});
                    const auto choice = m_decider->decide(req);
                    if (choice.instance_id.is_none())
                        return Option<PlayAction>::None();
                    return Option<PlayAction>::Some(PlayAction{
                        choice.instance_id.unwrap(), {}, choice.second_instance_id});
                }

                /**
                 * @brief  濒死救场适配：枚举救场牌并翻译选择。
                 * @param[in] ctx   只读容器视图。
                 * @param[in] saver 被询问的救援者 id。
                 * @param[in] dying 濒死者 id（`saver == dying` 时只找自救牌）。
                 * @return 打出的救场牌实例 id；`None` = 不救。
                 * @retval Some 引擎校验并负责消费该牌。
                 * @retval None 放弃救援。
                 * @post 本接口不改变任何状态。
                 */
                Option<std::string> play_peach(
                    const ReadOnlyContext &ctx, const std::string &saver,
                    const std::string &dying) override
                {
                    DecisionRequest req = base_request(ctx, saver);
                    req.kind = DecisionKind::Peach;
                    req.dying = dying;
                    const bool is_self = (saver == dying);
                    for (const auto &c : ctx.cards->hand(saver))
                        if (is_rescue_card(ctx, c, is_self))
                            req.options.push_back(c);
                    return m_decider->decide(req).instance_id;
                }

                /**
                 * @brief  无懈窗口适配：枚举无懈牌并翻译选择。
                 * @param[in] ctx            只读容器视图。
                 * @param[in] player         被询问的实体 id。
                 * @param[in] trick_user     锦囊使用者 id（延时判定窗为空串哨兵）。
                 * @param[in] trick_targets  锦囊目标集合。
                 * @param[in] trick_def_id   被无懈的锦囊 def id（空 = 未知）。
                 * @param[in] counter_played 本窗已打出的无懈张数（公开事实，0 起）。
                 * @return 打出的无懈实例 id；`None` = 不出。
                 * @retval Some 引擎校验并负责消费该牌。
                 * @retval None 放弃响应。
                 * @post 本接口不改变任何状态。
                 */
                Option<std::string> play_counter(
                    const ReadOnlyContext &ctx, const std::string &player,
                    const std::string &trick_user,
                    const std::vector<std::string> &trick_targets,
                    const std::string &trick_def_id,
                    int counter_played = 0) override
                {
                    DecisionRequest req = base_request(ctx, player);
                    req.kind = DecisionKind::Counter;
                    req.counter_user = trick_user;
                    req.counter_targets = trick_targets;
                    req.counter_trick = trick_def_id;
                    req.counter_played = counter_played;
                    for (const auto &c : ctx.cards->hand(player))
                        if (is_counter_card(ctx, c))
                            req.options.push_back(c);
                    return m_decider->decide(req).instance_id;
                }

                /**
                 * @brief  可选装备能力触发适配。
                 * @param[in] ctx     只读容器视图。
                 * @param[in] player  决策者 id。
                 * @param[in] ability 待触发的装备能力。
                 * @return true = 发动，false = 不发动。
                 * @post 本接口不改变任何状态。
                 */
                bool trigger_effect(
                    const ReadOnlyContext &ctx, const std::string &player,
                    card::Ability ability) override
                {
                    DecisionRequest req = base_request(ctx, player);
                    req.kind = DecisionKind::Trigger;
                    req.ability = ability;
                    return m_decider->decide(req).accepted;
                }

                /**
                 * @brief  武将触发技适配。
                 * @param[in] ctx    只读容器视图。
                 * @param[in] player 决策者 id。
                 * @param[in] skill  待触发的武将技能。
                 * @param[in] cause  触发来源 id（空 = 无来源）。
                 * @return true = 发动，false = 不发动。
                 * @post 本接口不改变任何状态。
                 */
                bool trigger_hero_skill(
                    const ReadOnlyContext &ctx, const std::string &player,
                    hero::HeroSkill skill, const std::string &cause) override
                {
                    DecisionRequest req = base_request(ctx, player);
                    req.kind = DecisionKind::Trigger;
                    req.hero_trigger = true;
                    req.hero_skill = skill;
                    req.trigger_cause = cause;
                    return m_decider->decide(req).accepted;
                }

                /**
                 * @brief  从目标区域选牌适配：隐藏手牌只放无身份占位槽。
                 * @param[in] ctx    只读容器视图。
                 * @param[in] source 决策者 id。
                 * @param[in] target 被选牌的目标 id。
                 * @param[in] scope  可选取域（手/装备/判定）。
                 * @return 选中的目标牌；`None` = 放弃或下标越界。
                 * @retval Some 隐藏手牌只回传槽位下标，真实身份由引擎随机暗抽。
                 * @retval None 放弃选择。
                 * @post 本接口不改变任何状态。
                 */
                Option<TargetPick> pick_card_from_target(
                    const ReadOnlyContext &ctx, const std::string &source,
                    const std::string &target, PickCardScope scope) override
                {
                    DecisionRequest req = base_request(ctx, source);
                    req.kind = DecisionKind::PickCard;
                    req.target = target;

                    // 隐藏手牌：只放无身份占位槽，数量与真实手牌一致，绝不拷贝身份
                    for (std::size_t i = 0; i < ctx.cards->hand_size(target); ++i)
                    {
                        req.options.push_back(card::Card{});
                        req.zone_labels.push_back(card::Zone::Hand);
                    }
                    append_zone(
                        req.options, req.zone_labels, ctx.cards->equip(target),
                        card::Zone::Equip);
                    if (scope == PickCardScope::HandEquipJudge)
                        append_zone(
                            req.options, req.zone_labels, ctx.cards->judge(target),
                            card::Zone::Judge);

                    const auto choice = m_decider->decide(req);
                    if (choice.option_index.is_none())
                        return Option<TargetPick>::None();
                    const std::size_t i = choice.option_index.unwrap();
                    if (i >= req.options.size())
                        return Option<TargetPick>::None();

                    const card::Zone zone = req.zone_labels[i];
                    // 隐藏手牌只回传槽位，真实身份由引擎随机暗抽
                    if (zone == card::Zone::Hand)
                        return Option<TargetPick>::Some(
                            TargetPick{card::Zone::Hand, i, Option<card::Card>::None()});
                    return Option<TargetPick>::Some(
                        TargetPick{zone, i, Option<card::Card>::Some(req.options[i])});
                }

                /**
                 * @brief  从亮出的牌中选牌适配。
                 * @param[in] ctx     只读容器视图。
                 * @param[in] player  决策者 id。
                 * @param[in] options 可选的亮牌。
                 * @param[in] source  亮牌来源结算。
                 * @return 选中的牌；`None` = 放弃或下标越界。
                 * @retval Some 引擎按来源消费该亮牌。
                 * @retval None 放弃选择。
                 * @post 本接口不改变任何状态。
                 */
                Option<card::Card> pick_from_revealed(
                    const ReadOnlyContext &ctx, const std::string &player,
                    const std::vector<card::Card> &options,
                    RevealSource source) override
                {
                    DecisionRequest req = base_request(ctx, player);
                    req.kind = DecisionKind::PickRevealed;
                    req.options = options;
                    req.reveal_source = source;

                    const auto choice = m_decider->decide(req);
                    if (choice.option_index.is_none())
                        return Option<card::Card>::None();
                    const std::size_t i = choice.option_index.unwrap();
                    if (i >= req.options.size())
                        return Option<card::Card>::None();
                    return Option<card::Card>::Some(req.options[i]);
                }

                /**
                 * @brief  出牌阶段适配：枚举合法动作并翻译选择。
                 * @param[in] ctx  只读容器视图。
                 * @param[in] turn 当前回合上下文（决策者 = `turn.player`）。
                 * @return 要执行的动作；`None` = 结束出牌阶段。
                 * @retval Some 引擎校验并结算该动作。
                 * @retval None 结束出牌阶段。
                 * @post 本接口不改变任何状态。
                 */
                Option<PlayAction> choose_play(
                    const ReadOnlyContext &ctx, const TurnContext &turn) override
                {
                    DecisionRequest req = base_request(ctx, turn.player);
                    req.kind = DecisionKind::Play;
                    req.turn = turn;
                    req.legal = legal_actions(ctx, turn.player, turn);
                    const auto choice = m_decider->decide(req);
                    if (choice.instance_id.is_none())
                        return Option<PlayAction>::None();
                    return Option<PlayAction>::Some(PlayAction{
                        choice.instance_id.unwrap(), choice.targets,
                        choice.second_instance_id, choice.recast,
                        choice.converted_sha});
                }

                /**
                 * @brief  弃牌适配：把手牌作为候选交给 Decider。
                 * @param[in] ctx    只读容器视图。
                 * @param[in] player 决策者 id。
                 * @param[in] count  需弃牌数。
                 * @param[in] reason 弃牌原因（回合上限/能力代价等）。
                 * @return 要弃置的牌实例 id 列表；数量不足由引擎按非法选择处理。
                 * @post 本接口不改变任何状态。
                 */
                std::vector<std::string> choose_discards(
                    const ReadOnlyContext &ctx, const std::string &player, int count,
                    DiscardReason reason) override
                {
                    DecisionRequest req = base_request(ctx, player);
                    req.kind = DecisionKind::Discard;
                    req.count = count;
                    req.discard_reason = reason;
                    req.options = ctx.cards->hand(player);
                    return m_decider->decide(req).discards;
                }

            private:
                Decider *m_decider; /**< 决策实现；生命周期由调用方保证覆盖本对象。 */

                /**
                 * @brief  构造只读基请求：填 actor/catalog/view，其余字段由各回调补齐。
                 * @param[in] ctx   只读容器视图。
                 * @param[in] actor 决策者 id。
                 * @return 已填公共字段的请求；观察为 `actor` 视角副本。
                 * @post 不改变任何状态。
                 */
                static DecisionRequest base_request(
                    const ReadOnlyContext &ctx, const std::string &actor)
                {
                    DecisionRequest req;
                    req.actor = actor;
                    req.catalog = ctx.catalog;
                    req.view = make_view(ctx, actor);
                    return req;
                }

                /**
                 * @brief  手牌是否可作指定类别的真响应牌。
                 * @param[in] ctx  只读容器视图。
                 * @param[in] c    待查手牌。
                 * @param[in] kind 响应牌类别。
                 * @return 命中该类别响应定义时为 true。
                 * @post 本接口不改变任何状态。
                 */
                static bool is_response_card(
                    const ReadOnlyContext &ctx, const card::Card &c,
                    card::ResponseKind kind)
                {
                    return StateQuery::hand_card_matching(
                        ctx, c, [kind](const card::CardDef &def)
                        { return is_response_def(def, kind); });
                }

                /**
                 * @brief  手牌是否可救场（自救助只认桃，救他人按定义口径）。
                 * @param[in] ctx     只读容器视图。
                 * @param[in] c       待查手牌。
                 * @param[in] is_self 救援者是否即濒死者。
                 * @return 可救场时为 true。
                 * @post 本接口不改变任何状态。
                 */
                static bool is_rescue_card(
                    const ReadOnlyContext &ctx, const card::Card &c, bool is_self)
                {
                    return StateQuery::hand_card_matching(
                        ctx, c,
                        [is_self](const card::CardDef &def)
                        { return can_rescue_def(def, is_self); });
                }

                /**
                 * @brief  手牌是否为无懈可击。
                 * @param[in] ctx 只读容器视图。
                 * @param[in] c   待查手牌。
                 * @return 该牌定义为无懈时为 true。
                 * @post 本接口不改变任何状态。
                 */
                static bool is_counter_card(
                    const ReadOnlyContext &ctx, const card::Card &c)
                {
                    return StateQuery::hand_card_matching(
                        ctx, c,
                        [](const card::CardDef &def) { return is_counter_def(def); });
                }

                /**
                 * @brief 追加一个区域的候选牌，并为每张牌记录来源分区。
                 * @param[in,out] out    候选牌输出。
                 * @param[in,out] labels 与 `out` 平行的来源分区标签输出。
                 * @param[in]     zone   待追加的区域牌。
                 * @param[in]     label  该区域的分区标签。
                 * @post `out` 与 `labels` 保持等长。
                 */
                static void append_zone(
                    std::vector<card::Card> &out,
                    std::vector<card::Zone> &labels,
                    const std::vector<card::Card> &zone, card::Zone label)
                {
                    out.insert(out.end(), zone.begin(), zone.end());
                    labels.insert(labels.end(), zone.size(), label);
                }
            };
        }
    }
}

#endif  // INCLUDE_TKW_GAME_DECIDER_HPP
