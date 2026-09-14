/**
 * @file decision.hpp
 * @brief 玩家决策源（策略接口）。
 * @details 结算器/回合流程需要「玩家做什么选择」时调用。实现即玩家策略：
 *          测试注入确定性假策略，CLI/TUI/网络层注入真人输入。响应牌/出牌的
 *          实际消费由结算器负责（单一写者），本接口只做决定。
 * @ingroup tkw_game_core
 */

#ifndef INCLUDE_TKW_GAME_DECISION_HPP
#define INCLUDE_TKW_GAME_DECISION_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "card/def.hpp"
#include "game/core/context.hpp"
#include "hero/def.hpp"

namespace tkw
{
    namespace game
    {
        /** @brief 手牌打出表达：打出一张手牌（可选第二张）并指定目标。 */
        struct PlayAction
        {
            std::string instance_id;          /**< 要打出的手牌 */
            std::vector<std::string> targets; /**< 目标实体 id（响应侧为空） */
            std::string second_instance_id;  /**< 第二张手牌（丈八蛇矛两张当杀；空 = 普通打出） */
            bool recast = false; /**< 重铸动作：弃置此牌并摸一张（空目标；不使用牌面效果） */
            bool converted_sha = false; /**< 单张转化当杀（武圣红牌 / 龙胆闪；来源由引擎按武将判定） */
        };

        /** @brief 弃牌的原因（同一接口在不同规则语境下的区分）。 */
        enum class DiscardReason : std::uint8_t
        {
            TurnLimit,     /**< 弃牌阶段：手牌超上限 */
            AbilityCost,   /**< 装备能力代价（如贯石斧弃两张） */
            CixiongChoice, /**< 雌雄双股剑二选一：空选择 = 令使用者摸一张 */
        };

        /**
         * @brief  弃牌原因 → 中文文案。
         * @details 弃牌窗口（REPL 与 TUI 面板）展示的稳定文案唯一事实源；新增原因值须
         *          同步本函数，避免各前端各写一份造成静默漂移。
         * @param[in] reason 弃牌原因。
         * @return 对应中文文案；未知名回落「弃牌」。
         * @post  不改变任何状态。
         */
        inline constexpr const char *discard_reason_text(DiscardReason reason)
        {
            switch (reason)
            {
            case DiscardReason::TurnLimit:
                return "手牌超上限";
            case DiscardReason::AbilityCost:
                return "装备能力代价";
            case DiscardReason::CixiongChoice:
                return "雌雄双股剑（可放弃）";
            }
            return "弃牌";
        }

        /**
         * @brief 出牌阶段的回合上下文（只读）：让决策源知道当前回合角色与
         *        已用「杀」次数，无需自行维护跨调用状态。
         */
        struct TurnContext
        {
            std::string player;  /**< 当前回合角色 id */
            int sha_played = 0;  /**< 本回合已使用的「杀」数 */
            int sha_limit = 1;   /**< 本回合「杀」上限（连弩为 INT_MAX） */
            bool analeptic_used = false; /**< 本回合出牌阶段是否已使用过酒 */
        };

        /** @brief 响应窗口的来源与后果（只读事实；来源未知时字段留空）。 */
        struct ResponsePrompt
        {
            std::string source_def_id; /**< 触发响应的牌 def id（空 = 未知） */
            std::string source_user;   /**< 来源使用者 id */
            int damage = 0;            /**< 不响应将受到的伤害量（0 = 不适用） */
        };

        /** @brief 亮牌选择的来源结算（决定窗口文案）。 */
        enum class RevealSource : std::uint8_t
        {
            Wugu,  /**< 五谷丰登：按座位序各选一张 */
            Qilin, /**< 麒麟弓：攻击方选弃目标坐骑 */
            FireAttackReveal,  /**< 火攻：目标本人展示一张手牌（强制选择） */
            FireAttackDiscard, /**< 火攻：使用者弃一张同花色手牌（可放弃） */
        };

        /** @brief 目标区域选牌的候选范围。 */
        enum class PickCardScope : std::uint8_t
        {
            HandEquipJudge, /**< 手牌 + 装备区 + 判定区（顺手牵羊/过河拆桥） */
            HandEquip,      /**< 仅手牌 + 装备区（寒冰剑；判定区延时锦囊不可取） */
        };

        /**
         * @brief 目标区域选牌结果：明置牌携带身份；隐藏手牌由引擎随机暗抽。
         * @note zone/index 始终是候选槽位坐标；card 仅在明置区（装备/判定）非空。
         *       对手手牌属隐藏信息，不进入决策面身份，引擎按 index 槽位结合当前
         *       手牌快照暗抽（见 resolve_target_pick）。
         */
        struct TargetPick
        {
            card::Zone zone = card::Zone::Hand; /**< 选中牌所在区域 */
            std::size_t index = 0;              /**< 候选槽位（无 rng 时确定性回落） */
            Option<card::Card> card = Option<card::Card>::None(); /**< 明置牌；隐藏手牌为 None */

            /**
             * @brief  逐字段相等比较。
             * @return 区域、槽位与明置牌三者均相等时为 true。
             */
            bool operator==(const TargetPick &) const = default;
        };

        /**
         * @class DecisionSource
         * @brief 结算/回合期间的玩家决策接口。
         * @note 只读：收到的 ReadOnlyContext 只聚合 const 容器指针，决策源
         *       从类型上无法改状态；所有落子（消费牌/改血/移除实体）都由引擎完成。
         */
        class DecisionSource
        {
        public:
            virtual ~DecisionSource() = default;

            /**
             * @brief  响应窗口：选择打出的响应牌（杀/闪）。
             * @details 结算器在需要响应时询问；实际校验与消费由结算器负责
             *          （单一写者）。
             * @param[in] ctx       只读容器视图（不含 EventBus/Rng）。
             * @param[in] entity_id 被询问的实体 id。
             * @param[in] kind      需要的响应牌类别。
             * @param[in] prompt    来源牌/使用者/不响应伤害量（只读事实，来源未知时留空）。
             * @return 要打出的手牌（可带第二张，两张手牌当杀）；`None` = 不响应。
             * @retval Some 引擎先校验手牌确含响应牌，并负责消费。
             * @retval None 放弃响应。
             * @pre   `ctx` 生命周期覆盖本次调用，`entity_id` 在 `ctx` 中可查。
             * @post  本接口不改变任何状态。
             * @note  响应侧的转化来源（武圣红牌/龙胆闪当杀、龙胆杀当闪）由引擎按
             *        「武将技能 + 所选牌」识别，回传的 `PlayAction` 无需置
             *        `converted_sha`。
             */
            virtual Option<PlayAction> play_response(
                const ReadOnlyContext &ctx,
                const std::string &entity_id,
                card::ResponseKind kind,
                const ResponsePrompt &prompt) = 0;

            /**
             * @brief  从目标区域选一张牌（过河拆桥弃置 / 顺手牵羊获得 / 寒冰剑弃置）。
             * @param[in] ctx    只读容器视图（不含 EventBus/Rng）。
             * @param[in] source 取牌方实体 id。
             * @param[in] target 被取牌的目标实体 id。
             * @param[in] scope  候选范围：顺手牵羊/过河拆桥取手牌+装备+判定区；
             *                   寒冰剑仅取手牌+装备区（判定区延时锦囊不可取）。
             * @return 选中的区域与槽位；`None` = 放弃/无可选。
             * @retval Some 明置牌（装备/判定）直接携带 `card`，隐藏手牌
             *              `card == None`，由引擎按槽位随机暗抽。
             * @retval None 放弃或无可选牌。
             * @pre   `ctx` 生命周期覆盖本次调用。
             * @post  本接口不改变任何状态。
             */
            virtual Option<TargetPick> pick_card_from_target(
                const ReadOnlyContext &ctx,
                const std::string &source,
                const std::string &target,
                PickCardScope scope) = 0;

            /**
             * @brief  出牌阶段：选择打出一张手牌及其目标。
             * @param[in] ctx  只读容器视图（不含 EventBus/Rng）。
             * @param[in] turn 当前回合上下文（只读）。
             * @return 要打出的牌及目标；`None` = 结束出牌阶段。
             * @retval Some 交由回合流程校验并落子。
             * @retval None 结束出牌阶段。
             * @note  回合流程负责校验合法性（手牌存在/目标合法/杀次数限制），
             *        不合法的动作会被拒绝并报错。`turn` 提供当前回合角色与已用
             *        杀次数，实现无需自行维护跨调用状态。
             * @post  本接口不改变任何状态。
             */
            virtual Option<PlayAction> choose_play(
                const ReadOnlyContext &ctx, const TurnContext &turn) = 0;

            /**
             * @brief  弃牌阶段/能力代价：弃置 count 张手牌。
             * @param[in] ctx    只读容器视图（不含 EventBus/Rng）。
             * @param[in] player 弃牌方实体 id。
             * @param[in] count  应弃置的张数。
             * @param[in] reason 弃牌原因（区分回合上限/能力代价/雌雄选择）。
             * @return 被选中弃置的手牌 instance_id 列表。
             * @note  回合流程按 count 逐张校验并弃置；数量不符/引用不存在会报错。
             * @post  本接口不改变任何状态。
             */
            virtual std::vector<std::string> choose_discards(
                const ReadOnlyContext &ctx, const std::string &player, int count,
                DiscardReason reason) = 0;

            /**
             * @brief  从候选牌中选一张（五谷丰登亮牌 / 麒麟弓选弃目标坐骑）。
             * @param[in] ctx     只读容器视图（不含 EventBus/Rng）。
             * @param[in] player  选择方实体 id。
             * @param[in] options 候选牌集合（只读事实）。
             * @param[in] source  亮牌来源结算（决定窗口文案，只读事实）。
             * @return 选中的牌，必须在 `options` 中；`None` = 放弃/非法。
             * @retval Some 引用 `options` 中一张牌。
             * @retval None 放弃或选择非法。
             * @note  成员校验由结算器按调用点执行：五谷丰登为强制选择，返回
             *        `None` 或引用不在 `options` 中即 InvalidChoice 整体失败；
             *        麒麟弓回落 `options` 首匹（发动即必弃一张）。
             * @post  本接口不改变任何状态。
             */
            virtual Option<card::Card> pick_from_revealed(
                const ReadOnlyContext &ctx, const std::string &player,
                const std::vector<card::Card> &options, RevealSource source) = 0;

            /**
             * @brief  濒死救场：saver 对濒死的 dying 打出哪张桃。
             * @param[in] ctx   只读容器视图（不含 EventBus/Rng）。
             * @param[in] saver 救场者实体 id。
             * @param[in] dying 濒死者实体 id。
             * @return 要打出的手牌 instance_id；`None` = 不救。
             * @retval Some 引擎先校验手牌确有救场牌，并负责消费。
             * @retval None 放弃救援。
             * @post  本接口不改变任何状态。
             */
            virtual Option<std::string> play_peach(
                const ReadOnlyContext &ctx, const std::string &saver,
                const std::string &dying) = 0;

            /**
             * @brief  无懈窗口：player 打出哪张无懈可击。
             * @param[in] ctx            只读容器视图（不含 EventBus/Rng）。
             * @param[in] player         被询问出牌者实体 id。
             * @param[in] trick_user     被结算锦囊的使用者；空串 = 延时锦囊判定
             *                           窗口（使用者不随牌记录，窗口主体为被判定
             *                           玩家）。
             * @param[in] trick_targets  锦囊目标集合（判定窗口 = 被判定玩家一人）。
             * @param[in] trick_def_id   被结算锦囊的 def id（只读事实；判定窗口亦携带）。
             * @param[in] counter_played 本窗已打出的无懈张数（公开事实，0 起；引擎
             *                           恒显式传值，直调测试可省略）。
             * @return 要打出的手牌 instance_id；`None` = 不出。
             * @retval Some 引擎先校验手牌确有牌，并负责消费。
             * @retval None 放弃打出。
             * @note  接缝只传事实（谁的锦囊、冲谁、本窗已出几张），不传「该不该
             *        出」的结论。
             * @post  本接口不改变任何状态。
             */
            virtual Option<std::string> play_counter(
                const ReadOnlyContext &ctx, const std::string &player,
                const std::string &trick_user,
                const std::vector<std::string> &trick_targets,
                const std::string &trick_def_id, int counter_played = 0) = 0;

            /**
             * @brief  装备效果触发：player 是否发动 ability 指定的可选装备能力。
             * @details 覆盖青龙偃月刀续杀 / 贯石斧弃两牌 / 麒麟弓弃马 / 寒冰剑免伤。
             * @param[in] ctx     只读容器视图（不含 EventBus/Rng）。
             * @param[in] player  被询问的实体 id。
             * @param[in] ability 待触发的装备能力。
             * @return true = 发动；false = 不发动。
             * @note  实现应只在「有牌可弃/有效果可用」时返回 true。
             * @post  本接口不改变任何状态。
             */
            virtual bool trigger_effect(
                const ReadOnlyContext &ctx, const std::string &player,
                card::Ability ability) = 0;

            /**
             * @brief  武将触发技是否发动：结算点直接询问（如受到伤害后的反馈）。
             * @param[in] ctx   只读容器视图（不含 EventBus/Rng）。
             * @param[in] player 被询问的实体 id。
             * @param[in] skill 触发的武将技能。
             * @param[in] cause 触发来源（如伤害来源实体 id；无来源时为空串）。
             * @return true = 发动；false = 不发动。
             * @note  与装备能力分属两条回调：语义不混用，装备走 `trigger_effect`；
             *        实现应只在有合法效果时返回 true。
             * @post  本接口不改变任何状态。
             */
            virtual bool trigger_hero_skill(
                const ReadOnlyContext &ctx, const std::string &player,
                hero::HeroSkill skill, const std::string &cause) = 0;
        };
    }
}

#endif  // INCLUDE_TKW_GAME_DECISION_HPP