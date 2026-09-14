/**
 * @file turn.hpp
 * @brief 回合流程：判定 → 摸牌 → 出牌（含杀次数限制/装备）→ 弃牌。
 * @note 规则约定：
 *       - 判定阶段按判定区顺序结算延时锦囊；乐不思蜀判定非红桃跳过出牌，
 *         兵粮寸断判定非梅花跳过摸牌，
 *         闪电判定黑桃2~9 则造成雷伤、否则移入判定区无同名闪电的下家；
 *       - 杀每回合限一次，装备诸葛连弩后不限制；
 *       - 弃牌阶段手牌上限 = 当前体力值。
 * @note 死亡/濒死救场不在本模块（hp 可被扣到非正，死亡声明归后续流程）。
 * @ingroup tkw_game_flow
 */

#ifndef INCLUDE_TKW_GAME_TURN_HPP
#define INCLUDE_TKW_GAME_TURN_HPP

#include <cstdint>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "card/card.hpp"
#include "card/def.hpp"
#include "card/manager.hpp"
#include "game/core/context.hpp"
#include "game/core/decision.hpp"
#include "game/core/effect.hpp"
#include "game/core/state.hpp"
#include "game/query/distance.hpp"
#include "game/query/equip.hpp"
#include "game/query/judge.hpp"
#include "game/resolve/combat.hpp"
#include "game/resolve/counter.hpp"
#include "game/resolve/resolver.hpp"
#include "util/types.hpp"

namespace tkw
{
    namespace game
    {
        /** @brief 回合流程错误。 */
        enum class TurnError : std::uint8_t
        {
            UnknownPlayer,       /**< 实体不存在。 */
            UnknownCard,         /**< 目录中找不到该卡定义。 */
            CardNotInHand,       /**< 要打出的牌不在手牌中。 */
            InvalidTarget,       /**< 目标不在合法目标集合内。 */
            ShaLimitExceeded,    /**< 本回合杀次数已达上限。 */
            AnalepticLimitExceeded, /**< 本回合已使用过酒（出牌阶段限一次）。 */
            NotEquipment,        /**< 装备动作目标不是装备牌。 */
            DelayedDuplicate,    /**< 判定区已有同名的延时锦囊。 */
            PlayRejected,        /**< 结算器拒绝该效果。 */
            DiscardInsufficient, /**< 弃牌数量不足或引用了不存在的牌。 */
            JudgeEmptyDeck,      /**< 判定时摸牌堆与弃牌堆皆空。 */
        };

        /**
         * @brief 回合流程结果别名。
         * @tparam T 成功时承载的值类型。
         */
        template <typename T>
        using TurnResult = Result<T, TurnError>;

        /** @brief 延时锦囊判定结果。 */
        enum class DelayedOutcome : std::uint8_t
        {
            Normal,          /**< 判定后无特殊效果（乐不思蜀为红桃）。 */
            SkipPlay,        /**< 跳过出牌阶段（乐不思蜀非红桃）。 */
            SkipDraw,        /**< 跳过摸牌阶段（兵粮寸断非梅花）。 */
            LightningStruck, /**< 闪电劈中。 */
            PassedToNext,    /**< 闪电未劈中，移至下家判定区。 */
        };

        /**
         * @brief 回合只读查询（静态工具类）。
         * @details 纯函数集合：不持有上下文，`ctx`/入参由调用方传入；本类不改变任何状态。
         */
        class TurnQuery
        {
        public:
            /** @brief 静态工具类，不可实例化。 */
            TurnQuery() = delete;

            /**
             * @brief  下家：按座位序环绕的存活玩家。
             * @param[in] ctx    只读上下文。
             * @param[in] player 参照玩家 id。
             * @return `player` 之后的第一个存活玩家 id；死亡者已被移除，天然跳过。
             */
            static std::string next_player(
                const GameContext &ctx, const std::string &player);

            /**
             * @brief  该定义是否为「杀」（效果类别 = `Damage`，单一事实源）。
             * @param[in] def 卡定义。
             * @return 效果类别为 `Damage` 时为 true。
             */
            static bool is_sha(const card::CardDef &def);

            /**
             * @brief  从手牌找一张牌。
             * @param[in] ctx         只读上下文。
             * @param[in] player      手牌所有者 id。
             * @param[in] instance_id 目标牌实例 id。
             * @return 查询结果。
             * @retval Some 命中牌副本，便于随后按 `instance_id` 消费。
             * @retval None 手牌中不存在该实例。
             */
            static Option<card::Card> find_in_hand(
                const GameContext &ctx, const std::string &player,
                const std::string &instance_id);

            /**
             * @brief  结算错误 → 回合错误（单一映射点）。
             * @param[in] e 结算错误。
             * @return 对应回合错误；未列明的值一律落 `TurnError::PlayRejected`。
             */
            static TurnError to_turn_error(EffectError e);

            /**
             * @brief  角色是否仍在场。
             * @param[in] ctx    只读上下文。
             * @param[in] player 角色 id。
             * @return 实体仍在容器中时为 true；回合中可能因闪电/决斗等死亡被移除。
             */
            static bool is_alive(const GameContext &ctx, const std::string &player);
        };

        /** @brief 判定阶段汇总的「跳过阶段」集合：乐不思蜀跳 play、兵粮寸断跳 draw。 */
        struct TurnSkips
        {
            bool skip_play = false; /**< 跳过出牌阶段（乐不思蜀非红桃）。 */
            bool skip_draw = false; /**< 跳过摸牌阶段（兵粮寸断非梅花）。 */
        };

        /**
         * @brief 回合流程（操作类）。
         * @details 持对局上下文与决策源（引用、非拥有），方法不再逐个传
         *          `ctx`/`ai`；覆盖判定、摸牌、出牌、弃牌四阶段与回合入口。
         * @warning 不拥有 `m_ctx`/`m_ai`：二者须比本对象存活更久；不得跨局复用。
         */
        class TurnFlow
        {
        public:
            /**
             * @brief  绑定对局上下文与决策源。
             * @param[in,out] ctx 对局上下文；本对象只持引用。
             * @param[in,out] ai  决策源；出牌/弃牌/无懈窗口经它询问。
             */
            TurnFlow(GameContext &ctx, DecisionSource &ai) : m_ctx(ctx), m_ai(ai) {}

            /**
             * @brief  执行 `player` 的一个完整回合：判定 → 摸牌 → 出牌 → 弃牌。
             * @details 四阶段依次为 `run_judgement_phase`、`run_draw_phase`（受
             *          `skip_draw` 抑制）、`run_play_phase`（受 `skip_play` 抑制）、
             *          `run_discard_phase`。
             * @param[in] player 当前回合角色 id。
             * @return 结算结果。
             * @retval Ok  回合正常结束，或角色在阶段中途死亡而终止。
             * @retval Err(TurnError) 某阶段失败，原样上抛。
             * @note  入口把 `player` 置入 `m_ctx.turn_player` 并在返回时还原（覆盖
             *         全部早退），供濒死询问等结算读取当前回合角色；离开本函数即回到
             *         「无回合上下文」。
             * @note  酒的伤害加成与「本回合已用酒」标记在入口清空、出口清空：二者是
             *         回合内运行时状态，不持久化，也不跨回合/跨玩家泄漏。
             * @note  角色在任意阶段死亡即终止本回合（死亡实体已被移除，阶段函数内均
             *         重新 `find` 以免悬垂指针）。
             * @warning 失败时不会回滚已落子的部分（判定/摸牌/出牌可能已结算），调用方
             *          须消费该回合（推进行程），不得以同一角色重入。
             * @see   run_judgement_phase, run_draw_phase, run_play_phase, run_discard_phase
             */
            TurnResult<void> execute_turn(const std::string &player);

            /**
             * @brief  结算玩家判定区的一张延时锦囊。
             * @details 判定牌进弃牌堆；延时牌按结果弃置或移入下家判定区，并从原判定
             *          区移除。判定结算前先开无懈窗口，被抵消则直接弃置。
             * @param[in] player  被判定玩家 id。
             * @param[in] delayed 待结算的延时锦囊牌。
             * @return 结算结果。
             * @retval Ok(DelayedOutcome) 已按判定行动结算。
             * @retval Err(TurnError::UnknownCard)    目录中找不到该卡定义。
             * @retval Err(TurnError::JudgeEmptyDeck) 判定时摸牌堆与弃牌堆皆空。
             */
            TurnResult<DelayedOutcome> resolve_delayed(
                const std::string &player, const card::Card &delayed);

            /**
             * @brief  装备动作：手牌装备到装备区；同槽位已有装备则先弃置旧装备。
             * @param[in,out] ctx    对局上下文。
             * @param[in]     player 装备者 id。
             * @param[in]     card   要装备的手牌。
             * @return 结算结果。
             * @retval Ok  装备成功，旧同槽位装备已弃置。
             * @retval Err(TurnError::NotEquipment)  该卡不是装备牌。
             * @retval Err(TurnError::CardNotInHand) 该牌不在手牌中。
             * @note  同槽位旧装备在取牌前先弃置；`CardNotInHand` 早退发生在旧装备
             *         已弃置之后。
             * @note  本动作不询问决策源，故为静态方法，只收 `ctx`。
             */
            static TurnResult<void> equip_card(
                GameContext &ctx, const std::string &player, const card::Card &card);

            /**
             * @brief  出牌阶段：循环向 `DecisionSource` 要动作直到结束。
             * @param[in] player 当前回合角色 id。
             * @return 结算结果。
             * @retval Ok  决策源不再出牌，或角色中途死亡（决斗自伤等）。
             * @retval Err(TurnError) 非法动作（手牌不存在/目标非法/超杀次数等）立即
             *         中止本回合。
             * @note  每轮重采样杀上限，回合中途装连弩当轮生效；重铸不计杀次数。
             */
            TurnResult<void> run_play_phase(const std::string &player);

        private:
            /**
             * @brief  打出延时锦囊：按 `judge.scope` 校验目标，置入其判定区。
             * @param[in] player  使用者 id。
             * @param[in] card    打出的延时锦囊手牌。
             * @param[in] targets 目标列表；数量必须恰为 1。
             * @return 结算结果。
             * @retval Ok  已置入目标判定区；被无懈抵消时该牌直接弃置。
             * @retval Err(TurnError::PlayRejected)     该牌不是延时锦囊。
             * @retval Err(TurnError::InvalidTarget)    目标数量不为 1 或目标不在合法范围。
             * @retval Err(TurnError::DelayedDuplicate) 目标判定区已有同名延时锦囊。
             * @retval Err(TurnError::CardNotInHand)    该牌不在手牌中。
             * @note  同名延时锦囊不可叠加；打出时开无懈窗口，被抵消则直接弃置。
             */
            TurnResult<void> place_delayed(
                const std::string &player, const card::Card &card,
                const std::vector<std::string> &targets);

            /**
             * @brief  判定阶段：按判定区顺序结算延时锦囊。
             * @param[in] player 当前回合角色 id。
             * @return 本回合需跳过的阶段集合（乐不思蜀非红桃 → `skip_play`，
             *         兵粮寸断非梅花 → `skip_draw`）。
             * @retval Ok(TurnSkips) 已完成判定区结算。
             * @retval Err(TurnError) 某张延时锦囊结算失败，原样上抛。
             * @note  角色中途死亡（如闪电劈死）时停止后续结算，调用方经 `is_alive`
             *         判断回合是否终止。
             */
            TurnResult<TurnSkips> run_judgement_phase(const std::string &player);

            /**
             * @brief  摸牌阶段：摸 `draw_phase_count` 张（基础 `rules.draw_per_turn`，
             *         英姿 +1）。
             * @param[in] player 当前回合角色 id。
             * @post  该角色手牌增加相应张数，并发布摸牌事件。
             */
            void run_draw_phase(const std::string &player);

            /**
             * @brief  弃牌阶段：手牌上限 = 当前体力值。
             * @param[in] player 当前回合角色 id。
             * @return 结算结果。
             * @retval Ok  手牌未超上限或已弃足。
             * @retval Err(TurnError::DiscardInsufficient) 弃牌数量不足或引用了不存在的牌。
             */
            TurnResult<void> run_discard_phase(const std::string &player);

            GameContext &m_ctx;   /**< 对局上下文（引用，非拥有）。 */
            DecisionSource &m_ai; /**< 决策源（引用，非拥有）。 */
        };

        inline std::string TurnQuery::next_player(
            const GameContext &ctx, const std::string &player)
        {
            return ctx.entities->next(player);
        }

        inline bool TurnQuery::is_sha(const card::CardDef &def)
        {
            return def.effect.is_some() && is_sha_kind(def.effect.unwrap().kind);
        }

        inline bool TurnQuery::is_alive(
            const GameContext &ctx, const std::string &player)
        {
            return ctx.entities->find(player).is_some();
        }

    }
}

#endif  // INCLUDE_TKW_GAME_TURN_HPP
