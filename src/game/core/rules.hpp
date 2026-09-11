/**
 * @file rules.hpp
 * @brief 对局规则常量：把散落在各结算函数里的魔法数集中到一处。
 * @note 这是「规则数值」的唯一事实源；引擎函数经 rules_of(ctx) 读取，
 *       测试/CLI 可构造不同 RulesConfig 注入 Game 来调参。
 */

#ifndef INCLUDE_TKW_GAME_RULES_HPP
#define INCLUDE_TKW_GAME_RULES_HPP

#include <cstdint>

namespace tkw
{
    namespace game
    {
        /** @brief 对局规则数值（默认值 = 标准多人乱斗）。 */
        struct RulesConfig
        {
            int draw_per_turn = 2;  /**< 每回合摸牌阶段摸牌数 */
            int sha_limit = 1;      /**< 每回合「杀」次数上限（连弩除外） */
            int kill_reward = 3;    /**< 击杀一名角色后的奖励摸牌数 */
            int initial_hand = 4;   /**< 开局每人初始手牌数 */
            int max_turns = 1000;   /**< 主循环回合上限（防僵局） */
            int dying_rounds = 64;  /**< 濒死救场轮上限（保险） */
            int wuxie_rounds = 32;  /**< 无懈可击链轮上限（保险） */
            int duel_rounds = 64;   /**< 决斗轮流出杀轮上限（保险） */
            int base_hp = 4;        /**< 玩家初始/上限体力 */
            int min_players = 2;    /**< 允许的最小玩家数 */
            int max_players = 8;    /**< 允许的最大玩家数 */
            int rescue_heal = 1;           /**< 濒死救回每张桃回复的体力 */
            int default_damage = 1;        /**< 响应杀无 effect 时的伤害回退 */
            int sha_multi_target_max = 3;  /**< 方天画戟杀最多目标数（含原目标） */
            int two_card_cost = 2;         /**< 贯石斧/寒冰剑「弃两张」能力代价 */
        };
    }
}

#endif  // INCLUDE_TKW_GAME_RULES_HPP
