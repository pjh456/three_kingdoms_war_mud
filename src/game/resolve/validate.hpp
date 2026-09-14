/**
 * @file validate.hpp
 * @brief 出牌动作只读校验：目标选择与合法性预检，供流程与 AI 枚举共用。
 * @details 与动作枚举（ai/legal.hpp）同源：枚举器产出的动作都应能通过本层
 *          校验。只读无副作用：不消费牌、不发事件；实际消费归 equip_card /
 *          place_delayed / resolve_play。校验与落子分离：新增效果先加校验分支，
 *          再加结算分支。
 * @ingroup tkw_game_resolve
 */

#ifndef INCLUDE_TKW_GAME_VALIDATE_HPP
#define INCLUDE_TKW_GAME_VALIDATE_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "card/def.hpp"
#include "card/manager.hpp"
#include "game/core/context.hpp"
#include "game/core/decision.hpp"
#include "game/core/effect.hpp"
#include "game/core/state.hpp"
#include "game/query/distance.hpp"
#include "game/query/equip.hpp"
#include "game/query/hero.hpp"
#include "game/query/judge.hpp"
#include "util/types.hpp"

namespace tkw
{
    namespace game
    {
        /**
         * @brief 出牌动作与效果结算的错误种类。
         * @see GameResult, validate_play_action, validate_effect_targets
         */
        enum class EffectError : std::uint8_t
        {
            UnknownCard,     /**< 目录中找不到该卡定义。 */
            UnsupportedKind, /**< 该 effect.kind 尚未实现。 */
            NoTarget,        /**< 需要至少一个目标。 */
            OutOfRange,      /**< 目标不在攻击范围/距离内。 */
            InvalidTarget,   /**< 目标数量不符 scope / 不在合法目标集合内。 */
            CardNotOwned,    /**< 打出的牌不在该玩家手牌中。 */
            InvalidChoice,   /**< 决策源选中的牌不存在于目标区域。 */
            ShaLimitExceeded, /**< 本回合杀次数已达上限。 */
            DelayedDuplicate, /**< 判定区已有同名的延时锦囊。 */
            AnalepticLimitExceeded, /**< 本回合已使用过酒（出牌阶段限一次）。 */
        };

        /**
         * @brief 结算结果别名：成功为 `T`，失败为 `EffectError`。
         * @tparam T 成功时的返回值类型。
         * @see EffectError, Result
         */
        template <typename T>
        using GameResult = Result<T, EffectError>;

        // ── 目标选择 ────────────────────────────────────────────────────

        /**
         * @brief 牌堆中的「杀」定义（Damage 类主动效果的卡，标准牌堆恰好一张）。
         * @param[in] ctx 只读上下文。
         * @return 杀定义；`None` = 牌堆无杀（虚拟杀无从结算）。
         * @retval Some 按 deck 序首个满足 `is_sha_kind` 的卡定义指针。
         * @retval None 目录为空或不存在 Damage 类主动效果卡。
         * @note 虚拟杀（两张手牌当一张杀）借用其效果参数（伤害量/作用范围），
         *       牌堆含多张 Damage 卡时取 deck 序第一张。
         */
        Option<const card::CardDef *> find_sha_def(const ReadOnlyContext &ctx);

        /**
         * @brief 按 effect.scope 返回该牌在当前局面下的合法目标集合。
         * @param[in] ctx    只读上下文。
         * @param[in] player 使用者实体 id。
         * @param[in] def    卡牌定义（消费其 effect.scope / kind / range）。
         * @return 合法目标 id 列表；无主动效果时为空。
         * @note 含距离过滤：杀按攻击范围、顺手牵羊按锦囊距离（奇才无视）。只读，
         *       不消费牌、不发事件。
         * @pre   `def.effect.is_some()` 由调用方保证时才有意义；否则返回空集合。
         */
        std::vector<std::string> valid_targets(
            const ReadOnlyContext &ctx, const std::string &player, const card::CardDef &def);

        /**
         * @brief 主动效果的目标预校验（只读，不消费打出的牌）。
         * @param[in] ctx            只读上下文。
         * @param[in] player         使用者实体 id。
         * @param[in] def            卡牌定义（消费其 effect.scope / kind / range）。
         * @param[in] targets        声明的目标 id 列表。
         * @param[in] cards_consumed 该效果消耗的手牌张数（真杀 1，丈八虚拟杀 2），
         *                           参与方天画戟「最后手牌」放宽判定。
         * @return 目标是否合法。
         * @retval Ok     目标通过全部检查，可进入消费与结算。
         * @retval Err(EffectError::NoTarget) 无目标。
         * @retval Err(EffectError::OutOfRange) 目标超出攻击范围/距离。
         * @retval Err(EffectError::InvalidTarget) 目标数量不符 scope 或不在合法集合内。
         * @pre   结算入口与出牌动作校验两处均满足 `def.effect.is_some()`。
         * @post  只读：不消费牌、不发事件、不改变任何状态。
         * @note 借刀杀人特例（targets = {持武器者, 其攻击范围内另一名角色}）在此统一校验。
         *       方天画戟放宽：杀的目标为唯一目标且该杀消耗完手中全部牌时，
         *       OneOther 数量上限放宽为 3（额外至多 2 名，卡面）。
         */
        GameResult<void> validate_effect_targets(
            const ReadOnlyContext &ctx, const std::string &player,
            const card::CardDef &def, const std::vector<std::string> &targets,
            std::size_t cards_consumed = 1);

        /**
         * @brief 校验「player 打出 card（定义 def），指定 targets」是否合法（只读预检）。
         * @param[in] ctx     只读上下文。
         * @param[in] player  出牌实体 id。
         * @param[in] def     卡牌定义。
         * @param[in] card    待打出的手牌对象。
         * @param[in] targets 声明的目标 id 列表。
         * @param[in] turn    当前回合上下文（杀/酒次数簿记）。
         * @return 动作是否合法。
         * @retval Ok     合法，可进入消费与结算。
         * @retval Err(EffectError::CardNotOwned) 牌不在 player 手牌中。
         * @retval Err(EffectError::InvalidTarget) 延时锦囊目标数不为 1 或出 scope。
         * @retval Err(EffectError::DelayedDuplicate) 延时锦囊目标判定区已有同名延时锦囊。
         * @retval Err(EffectError::ShaLimitExceeded) 杀且本回合杀次数已达上限（turn）。
         * @retval Err(EffectError::AnalepticLimitExceeded) 酒且本回合已使用过酒（turn）。
         * @retval Err(EffectError::NoTarget/OutOfRange/InvalidTarget) 主动效果目标校验失败。
         * @retval Err(EffectError::UnsupportedKind) 无主动效果，或效果未实现。
         * @post  只读：不消费牌、不发事件、不改变任何状态。
         * @note 检查顺序固定：手牌存在 → 按分类分派（装备直接合法、忽略目标；
         *       延时先 scope 后去重；主动先杀次数后目标再可实现性）→ 主动效果目标
         *       校验 → 可实现性。实际消费归 equip_card / place_delayed / resolve_play。
         */
        GameResult<void> validate_play_action(
            const ReadOnlyContext &ctx, const std::string &player,
            const card::CardDef &def, const card::Card &card,
            const std::vector<std::string> &targets, const TurnContext &turn);

        /**
         * @brief 校验「虚拟杀」是否合法（只读预检）。
         * @param[in] ctx       只读上下文。
         * @param[in] player    出牌实体 id。
         * @param[in] first_id  虚拟杀的第一张来源手牌 instance_id。
         * @param[in] second_id 第二张来源手牌 instance_id；非空 = 丈八蛇矛两张当杀，
         *                      为空 = 单张转化（武圣红牌 / 龙胆闪）。
         * @param[in] targets   声明的目标 id 列表。
         * @param[in] turn      当前回合上下文（杀次数簿记）。
         * @return 虚拟杀动作是否合法。
         * @retval Ok     合法，可进入消费与结算。
         * @retval Err(EffectError::CardNotOwned) 两牌为同一张或任一（单张路径为 first）
         *         不在 player 手牌中。
         * @retval Err(EffectError::UnsupportedKind) 来源不满足（丈八未装备两张当杀能力 /
         *         单张不满足 can_convert_card_to_sha），或牌堆无「杀」定义。
         * @retval Err(EffectError::ShaLimitExceeded) 杀且本回合杀次数已达上限（turn）。
         * @retval Err(EffectError::NoTarget/OutOfRange/InvalidTarget) 目标校验失败
         *         （攻击范围内的一名其他角色；方天画戟放宽与杀一致，按消耗张数）。
         * @post  只读：不消费牌、不发事件、不改变任何状态。
         * @note 检查顺序：两牌在手 → 来源能力/花色 → 杀次数 → 目标；实际消费归
         *       `resolve_virtual_sha`。
         */
        GameResult<void> validate_virtual_sha(
            const ReadOnlyContext &ctx, const std::string &player,
            const std::string &first_id, const std::string &second_id,
            const std::vector<std::string> &targets, const TurnContext &turn);
    }
}

#endif  // INCLUDE_TKW_GAME_VALIDATE_HPP
