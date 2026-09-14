/**
 * @file   aggressive.hpp
 * @brief  攻击优先策略：伤害/多目标先行的决策档，供 CLI `--ai aggressive` 使用。
 * @details 逻辑集中在 `AggressiveDecider::decide`（只读 `DecisionRequest`）；
 *          `AggressiveAI` 把它经 `RequestDecisionSource` 适配成引擎可用的
 *          `DecisionSource`。相对贪心档的难度定位：出牌阶段按「卡类优先级」选组打出
 *          （小者先打，顺序见 `kPlayPriority*` 常量，同分取 legal 序靠前者）；选牌
 *          分支（PickCard/PickRevealed）取最高牌价值而非首张；PickCard 的对手手牌为
 *          无身份占位槽，仅按期望常量参与比较，不读取真实身份。其余六个分支
 *          （响应/救桃/无懈/触发/弃牌/丈八组目标）与贪心档同逻辑。身份局叠加阵营
 *          意识：出牌跳过只打友方的有害组，无懈只挡敌方冲自己/友方，救桃按阵营取舍，
 *          内奸避让并救主公直到主公成为最后一名非内奸；乱斗与无角色时全部回落旧
 *          口径。无随机、无隐藏状态，确定性可回放。
 * @warning 只读 AI：实现不得修改对局状态或缓存请求内指针。
 * @ingroup tkw_game_ai
 */

#ifndef INCLUDE_TKW_GAME_AGGRESSIVE_HPP
#define INCLUDE_TKW_GAME_AGGRESSIVE_HPP

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "card/card.hpp"
#include "card/def.hpp"
#include "game/ai/decider.hpp"
#include "game/ai/decider_base.hpp"
#include "util/types.hpp"

namespace tkw
{
    namespace game
    {
        namespace ai
        {
            /**
             * @brief 攻击优先决策逻辑（卡类优先级出牌 + 集火最低体力）。
             * @warning 只读：全部决策只读 `DecisionRequest`，不修改对局状态。
             */
            class AggressiveDecider : public DeciderBase
            {
            public:
                /**
                 * @brief  攻击优先决策入口：公共分支走基类分派，出牌与选牌用本档策略。
                 * @param[in] req 决策请求。
                 * @return 对应类别的决策；空结果表示放弃。
                 * @post 不改变任何状态。
                 */
                DecisionChoice decide(const DecisionRequest &req) override
                {
                    return dispatch(req, decide_play, decide_best_card);
                }

            private:
                // 卡类出牌优先级（小者先打；同分取 legal 序靠前者）；-1 = 跳过该组。
                static constexpr int kPlayPriorityAnaleptic = 0;     /**< 酒（先饮酒再出杀） */
                static constexpr int kPlayPriorityDamage = 1;        /**< 伤害/虚拟杀（丈八两张、武圣单张） */
                static constexpr int kPlayPriorityAoe = 2;           /**< 群体伤害 */
                static constexpr int kPlayPriorityDelayed = 3;       /**< 延时锦囊 */
                static constexpr int kPlayPriorityDuel = 4;          /**< 决斗 */
                static constexpr int kPlayPriorityDiscard = 5;       /**< 过河拆桥 */
                static constexpr int kPlayPrioritySteal = 6;         /**< 顺手牵羊 */
                static constexpr int kPlayPriorityDraw = 7;          /**< 无中生有 */
                static constexpr int kPlayPriorityBorrowedSword = 8; /**< 借刀杀人 */
                static constexpr int kPlayPriorityHeal = 9;          /**< 桃（满血跳过） */
                static constexpr int kPlayPriorityOther = 10;        /**< 装备/无效果/亮牌/未知兜底 */
                static constexpr int kPlayPrioritySkip = -1;         /**< 跳过（缺 def / 满血自疗） */

                /**
                 * @brief 选牌（PickCard/PickRevealed）：取牌价值最高者；同值保持
                 *        候选序靠前者；空候选返回 None，仅回传候选下标。
                 * @param[in] req 选牌类决策请求（候选在 `options`）。
                 * @return 最高价值候选的下标决策；候选为空时返回空选择。
                 * @post 不改变任何状态。
                 * @note PickCard 的对手手牌为无身份占位槽，按期望常量估值，无法
                 *       识别/挑选具体手牌；装备/判定区等明置牌按真实牌价值比较，
                 *       五谷丰登亮牌高价值优先。
                 */
                static DecisionChoice decide_best_card(const DecisionRequest &req);

                /**
                 * @brief 出牌：按牌分组（legal 首现序 = 手牌序），按卡类优先级
                 *        选出牌组（小者先打，同分取 legal 序靠前者），再组内选目标。
                 * @param[in] req 出牌类决策请求（合法动作在 `legal`）。
                 * @return 要执行的动作；全组可跳过时返回空选择（结束出牌阶段）。
                 * @post 不改变任何状态。
                 * @note 虚拟杀组（丈八第二张 / 武圣单张转化）按伤害对待；手牌有
                 *       真杀时单张转化动作不参与分组（真杀正常打出，避免烧牌）；
                 *       满血自疗与目录缺失的组跳过；全跳过时返回空 Choice =
                 *       结束出牌阶段。
                 */
                static DecisionChoice decide_play(const DecisionRequest &req);

                /**
                 * @brief 已选组内选目标。
                 * @param[in] req  出牌类决策请求（供目标选择取观察）。
                 * @param[in] opts 该组全部合法动作。
                 * @param[in] def  组代表卡的定义；虚拟杀组允许为空（按伤害结算，
                 *        不查目录），非虚拟杀组调用前已保证非空。
                 * @return 该组的决策。
                 * @pre   非虚拟杀组时 `def` 非空且 `opts` 非空。
                 * @post  不改变任何状态。
                 * @note 虚拟杀组（丈八第二张 / 武圣单张转化）按伤害接管；其余
                 *       目标选择委托共享尾段，组内虚拟杀扫描范围是本档与贪心档
                 *       的有意分歧，故留在本处。
                 */
                static DecisionChoice decide_play_group(
                    const DecisionRequest &req, const std::vector<LegalAction> &opts,
                    const card::CardDef *def);

                /**
                 * @brief 卡组优先级：含虚拟杀动作的组按伤害档；否则按组首张卡的
                 *        定义分派。
                 * @param[in] req  出牌类决策请求（供目录查询与身份过滤）。
                 * @param[in] opts 同一张牌的合法动作组。
                 * @return 卡类优先级；`kPlayPrioritySkip` = 该组跳过。
                 * @retval kPlayPrioritySkip 目录缺失、满血自疗或整组只打友方。
                 * @post 不改变任何状态。
                 * @note 虚拟杀 pair 仅在无真杀时产出，单张转化动作在有真杀时已被
                 *       剔除，与真杀同优先级无冲突。
                 */
                static int group_priority(
                    const DecisionRequest &req,
                    const std::vector<LegalAction> &opts);

                /**
                 * @brief 卡类优先级（小者先打）：顺序见 kPlayPriority* 常量。
                 * @param[in] req 出牌类决策请求（供观察体力与身份）。
                 * @param[in] def 卡牌定义。
                 * @return 卡类优先级；`kPlayPrioritySkip` = 跳过。
                 * @retval kPlayPrioritySkip 满血自疗（同贪心档「满血不打桃」）。
                 * @post 不改变任何状态。
                 * @note 无主动效果的锦囊（延时判定牌）排装备之前；响应牌不会
                 *       进入 legal，归入兜底档。
                 */
                static int play_priority(
                    const DecisionRequest &req, const card::CardDef &def);
            };

            /** @brief 引擎可用的攻击优先决策源：AggressiveDecider 经适配器接入。 */
            class AggressiveAI : private AggressiveDecider,
                                  public RequestDecisionSource
            {
            public:
                /**
                 * @brief 构造：把自身作为决策实现接入适配器。
                 * @post 对象持有对自身 `AggressiveDecider` 子对象的指针。
                 */
                AggressiveAI() :
                    RequestDecisionSource(static_cast<AggressiveDecider &>(*this))
                {
                }
            };
        }

        /** @brief AggressiveAI 在 game 命名空间的别名。 */
        using ai::AggressiveAI;
    }
}

#endif  // INCLUDE_TKW_GAME_AGGRESSIVE_HPP
