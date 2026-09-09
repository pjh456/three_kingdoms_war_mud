/**
 * @file decision.hpp
 * @brief 玩家决策源（策略接口）：结算器/回合流程需要「玩家做什么选择」时调用。
 * @note 实现即玩家策略：测试注入确定性假策略，将来 CLI/网络层注入真人输入。
 *       响应牌/出牌的**实际消费**由结算器负责（单一写者），本接口只做决定。
 */

#ifndef INCLUDE_TKW_GAME_DECISION_HPP
#define INCLUDE_TKW_GAME_DECISION_HPP

#include <string>
#include <vector>

#include "card/def.hpp"
#include "game/core/context.hpp"

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
        };

        /** @brief 弃牌的原因（同一接口在不同规则语境下的区分）。 */
        enum class DiscardReason : std::uint8_t
        {
            TurnLimit,   /**< 弃牌阶段：手牌超上限 */
            AbilityCost, /**< 装备能力代价（如贯石斧弃两张） */
        };

        /**
         * @brief 出牌阶段的回合上下文（只读）：让决策源知道当前回合角色与
         *        已用「杀」次数，无需自行维护跨调用状态。
         */
        struct TurnContext
        {
            std::string player;  /**< 当前回合角色 id */
            int sha_played = 0;  /**< 本回合已使用的「杀」数 */
            int sha_limit = 1;   /**< 本回合「杀」上限（连弩为 INT_MAX） */
        };

        /**
         * @class DecisionSource
         * @brief 结算/回合期间的玩家决策接口。
         * @note 只读：收到的 GameContext 为 const，决策源不得直接改状态；
         *       所有落子（消费牌/改血/移除实体）都由引擎完成。
         */
        class DecisionSource
        {
        public:
            virtual ~DecisionSource() = default;

            /**
             * @brief 响应窗口：entity_id 选择打出的响应牌（杀/闪）。
             * @return 要打出的手牌（可带第二张，两张手牌当杀）；None = 不响应。
             *         结算器会先检查手牌里确有响应牌，并负责消费。
             */
            virtual Option<PlayAction> play_response(
                const GameContext &ctx,
                const std::string &entity_id,
                card::ResponseKind kind) = 0;

            /**
             * @brief 从目标区域选一张牌（过河拆桥弃置 / 顺手牵羊获得）。
             * @return 选中的牌；None = 放弃/无可选（结算器按规则处理，不再
             *         依赖默认构造的牌）。
             */
            virtual Option<card::Card> pick_card_from_target(
                const GameContext &ctx,
                const std::string &source,
                const std::string &target) = 0;

            /**
             * @brief 出牌阶段：选择打出一张手牌及其目标；None = 结束出牌。
             * @note 回合流程负责校验合法性（手牌存在/目标合法/杀次数限制），
             *       不合法的动作会被拒绝并报错。turn 提供当前回合角色与已用
             *       杀次数，实现无需自行维护跨调用状态。
             */
            virtual Option<PlayAction> choose_play(
                const GameContext &ctx, const TurnContext &turn) = 0;

            /**
             * @brief 弃牌阶段/能力代价：弃置 count 张手牌。
             * @note 回合流程按 count 逐张校验并弃置；数量不符/引用不存在会报错。
             */
            virtual std::vector<std::string> choose_discards(
                const GameContext &ctx, const std::string &player, int count,
                DiscardReason reason) = 0;

            /**
             * @brief 从亮出的若干张牌中选一张（五谷丰登）。
             * @return 选中的牌；None = 放弃/非法（结算器回落到第一张）。
             */
            virtual Option<card::Card> pick_from_revealed(
                const GameContext &ctx, const std::string &player,
                const std::vector<card::Card> &options) = 0;

            /**
             * @brief 濒死救场：saver 对濒死的 dying 打出哪张桃。
             * @return 要打出的手牌 instance_id；None = 不救。结算器先检查手牌
             *         确有救场牌再询问，并负责消费。
             */
            virtual Option<std::string> play_peach(
                const GameContext &ctx, const std::string &saver,
                const std::string &dying) = 0;

            /**
             * @brief 无懈窗口：player 打出哪张无懈可击。
             * @param trick_user 被结算锦囊的使用者；空串 = 延时锦囊判定窗口
             *        （使用者不随牌记录，窗口主体为被判定玩家）。
             * @param trick_targets 锦囊目标集合（判定窗口 = 被判定玩家一人）。
             * @return 要打出的手牌 instance_id；None = 不出。结算器先检查手牌
             *         确有牌再询问，并负责消费。
             * @note 接缝只传事实（谁的锦囊、冲谁），不传「该不该出」的结论。
             */
            virtual Option<std::string> play_counter(
                const GameContext &ctx, const std::string &player,
                const std::string &trick_user,
                const std::vector<std::string> &trick_targets) = 0;

            /**
             * @brief 装备效果触发：player 是否发动 ability 指定的可选装备能力
             *       （青龙偃月刀续杀 / 贯石斧弃两牌 / 麒麟弓弃马 / 寒冰剑免伤）。
             * @note 实现应只在「有牌可弃/有效果可用」时返回 true。
             */
            virtual bool trigger_effect(
                const GameContext &ctx, const std::string &player,
                card::Ability ability) = 0;
        };
    }
}

#endif  // INCLUDE_TKW_GAME_DECISION_HPP