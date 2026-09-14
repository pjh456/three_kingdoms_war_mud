/**
 * @file   decider_base.hpp
 * @brief  AI 决策档共享助手：响应/救桃/无懈/触发/弃牌/丈八/目标选择等纯函数。
 * @details 只承载与档位无关、只读 `DecisionRequest`/`AiView` 的静态助手；出牌组选择、
 *          选牌策略与优先级表是档位语义，保留在各派生 `Decider`。助手全为静态，
 *          不新增虚函数、无实例状态，定义在 `decider_base.cpp`；头内仅声明。
 * @ingroup tkw_game_ai
 */

#ifndef INCLUDE_TKW_GAME_DECIDER_BASE_HPP
#define INCLUDE_TKW_GAME_DECIDER_BASE_HPP

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include "card/card.hpp"
#include "card/def.hpp"
#include "game/ai/decider.hpp"
#include "game/ai/evaluator.hpp"
#include "game/ai/legal.hpp"
#include "game/ai/view.hpp"
#include "util/types.hpp"

namespace tkw
{
    namespace game
    {
        namespace ai
        {
            /**
             * @class DeciderBase
             * @brief 决策档共享基类：集中各档逐字相同的静态决策助手。
             * @note 仍为抽象类（decide 未实现）；派生档只实现档位特有逻辑并复用
             *       此处助手，规则修复改一处即两档同步。
             * @warning 全部助手只读请求与观察，不得修改对局状态。
             */
            class DeciderBase : public Decider
            {
            protected:
                /**
                 * @brief 响应窗口：真响应牌（闪/杀）取第一张候选；杀响应无真杀时
                 *        取首个两张当杀 pair（丈八蛇矛，真杀优先与主动侧一致）。
                 * @param[in] req 响应类决策请求（候选在 `options`/`legal`）。
                 * @return 选中的响应；候选与 pair 均为空时返回空选择。
                 * @retval instance_id 命中真响应牌或 pair 首张实例 id。
                 * @retval empty 无可用响应，放弃。
                 * @post 不改变任何状态。
                 */
                static DecisionChoice decide_response(const DecisionRequest &req);

                /**
                 * @brief  救桃取第一张候选。
                 * @param[in] req 救场类决策请求（候选在 `options`）。
                 * @return 选中的救场牌；候选为空时返回空选择。
                 * @retval instance_id 首个候选的实例 id。
                 * @retval empty 无候选，放弃。
                 * @post 不改变任何状态。
                 */
                static DecisionChoice decide_first_id(const DecisionRequest &req);

                /**
                 * @brief 决策分派：按 DecisionKind 收敛八类决策的公共骨架。
                 * @param[in] req  决策请求；`req.kind` 决定分支。
                 * @param[in] play 派生的出牌选择（档位语义：手牌序首组 / 卡类优先级）。
                 * @param[in] pick 派生的选牌策略（档位语义：首张 / 最高价值）。
                 * @return 对应分支的决策；未匹配值回落空 DecisionChoice。
                 * @post 不改变任何状态。
                 * @note 公共分支（响应/救桃/无懈/触发/弃牌）两档逐字相同，
                 *       仅出牌与选牌按档位策略分流，故以函数指针注入。
                 */
                static DecisionChoice dispatch(
                    const DecisionRequest &req,
                    DecisionChoice (*play)(const DecisionRequest &),
                    DecisionChoice (*pick)(const DecisionRequest &));

                /**
                 * @brief 无懈窗口：乱斗或无角色时回落旧口径（仅当锦囊冲自己
                 *        时出）；身份局按阵营判断——使用者为空（判定窗）或为
                 *        敌方时，敌方锦囊冲自己必出、冲友方时仅窗内首位保护者
                 *        出；友方锦囊一律不出（不拆自家人的牌）。敌方有益自益
                 *        锦囊（窗口唯一目标 = 使用者）按窗口奇偶补一张：仅当本
                 *        窗尚未被抵消（已出张数为偶）时出手。
                 * @param[in] req 无懈类决策请求（候选在 `options`）。
                 * @return 是否打出无懈，命中时填 `instance_id`。
                 * @retval instance_id 打出 `options.front()`（未出 = 空选择）。
                 * @retval empty 不打出。
                 * @post 不改变任何状态。
                 * @note 候选已由适配器滤为无懈牌且窗口询问前保证非空；直调
                 *       decide 时按不出处理空候选。多目标窗（借刀等）与使用者
                 *       角色未知时回落旧口径，避免误读奇偶链。
                 */
                static DecisionChoice decide_counter(const DecisionRequest &req);

                /**
                 * @brief 旧无懈口径：锦囊目标含决策者时出第一张候选，其余不出。
                 * @param[in] req 无懈类决策请求。
                 * @return 是否打出无懈，命中时填 `instance_id`。
                 * @retval instance_id 打出 `options.front()`（目标含决策者时）。
                 * @retval empty 目标不含决策者或无候选。
                 * @post 不改变任何状态。
                 * @note 乱斗、身份局角色缺失、多目标窗与未知使用者时的回退，
                 *       保持既有逐字节行为。
                 */
                static DecisionChoice legacy_counter(const DecisionRequest &req);

                /**
                 * @brief 濒死救场：乱斗或无角色回落旧口径（有救场牌就出）；
                 *        身份局按自身与濒死者的阵营决定是否救。
                 * @param[in] req 救场类决策请求（濒死者 = `req.dying`）。
                 * @return 打出的救场牌；`None` = 不救。
                 * @retval instance_id 首个救场候选实例 id。
                 * @retval empty 不应救或无候选。
                 * @post 不改变任何状态。
                 * @note 濒死者身份公开，角色经既有观察取得，不新增隐藏信息面。
                 */
                static DecisionChoice decide_peach(const DecisionRequest &req);

                /**
                 * @brief 是否应救濒死者：乱斗/无角色恒救；身份局主公阵营救
                 *        主公与忠臣，反贼只救反贼，内奸救自己并（主公尚未成为
                 *        最后一名非内奸时）救主公以维持制衡。
                 * @param[in] view  救者观察（含自身与其他角色）。
                 * @param[in] dying 濒死者 id。
                 * @return true = 打出救场牌。
                 * @post 不改变任何状态。
                 */
                static bool should_save(const AiView &view, const std::string &dying);

                /**
                 * @brief 触发：按代价可付性决定是否发动装备能力。
                 * @param[in] req 触发类决策请求（含能力/武将技能与观察手牌）。
                 * @return `accepted` 置位的决策（true = 发动）。
                 * @retval accepted=true  免费能力或手牌足以支付代价。
                 * @retval accepted=false 贯石斧手牌不足 2 张。
                 * @post 不改变任何状态。
                 * @note 贯石斧需弃两张，手牌不足 2 张不发动；免费能力一律发动
                 *       （目标侧代价在接缝内不可知，由引擎侧预检兜底）。
                 * @note 武将触发技（hero_trigger）恒发动：首片反馈为纯收益、无
                 *       可付代价，不新增档位分化。
                 */
                static DecisionChoice decide_trigger(const DecisionRequest &req);

                /**
                 * @brief 弃牌：候选按牌价值升序稳定排序（先弃最低价值），
                 *        同价值保持手牌序，取前 count 张。
                 * @param[in] req 弃牌类决策请求（候选在 `options`，数量 = `count`）。
                 * @return 按序填入 `discards` 的决策；候选不足时返回全部候选。
                 * @post 不改变任何状态。
                 * @note 目录缺失时全部价值为 0，稳定排序退化回手牌原序。
                 */
                static DecisionChoice decide_discard(const DecisionRequest &req);

                /**
                 * @brief 虚拟杀（丈八两张 / 武圣单张）：多目标动作取目标最多者
                 *        （方天画戟），否则集火最低体力；牌取首个枚举动作
                 *        （确定性）。
                 * @param[in] req  出牌类决策请求（供集火取观察）。
                 * @param[in] acts 虚拟杀动作集合；调用前保证非空。
                 * @return 选定的虚拟杀决策（实例 id、第二张、转化标记与目标）。
                 * @pre   `acts` 非空。
                 * @post 不改变任何状态。
                 */
                static DecisionChoice decide_zhangba(
                    const DecisionRequest &req, const std::vector<LegalAction> &acts);

                /**
                 * @brief 已选出牌组的目标选择尾段：装备/延时锦囊、借刀、单目标、
                 *        多目标。
                 * @param[in] req  出牌类决策请求（供目标选择取观察）。
                 * @param[in] opts 该组全部合法动作（组首卡 = opts.front().card）。
                 * @param[in] def  组首卡定义，调用前保证非空（见各档前置检查）。
                 * @return 该组的决策。
                 * @pre   `def` 非空且 `opts` 非空。
                 * @post 不改变任何状态。
                 * @note 不含丈八扫描与满血自疗跳过（两档分歧，留在派生类）；
                 *       多目标动作（方天画戟）取目标最多者，否则集火最低体力。
                 * @note 重铸候选（空目标）与正常动作同卡同组：目标选择只看带目标的
                 *       候选，避免对空 targets 取 front；组内全无带目标候选时该卡无
                 *       正常动作，唯一合法解即重铸，按空目标动作返回。
                 */
                static DecisionChoice select_group_targets(
                    const DecisionRequest &req, const std::vector<LegalAction> &opts,
                    const card::CardDef &def);

                /**
                 * @brief  单目标动作：填入首卡实例 id，并按集火规则选一个目标。
                 * @param[in] view 观察（供目标优先级与体力比较）。
                 * @param[in] out  已部分填好的决策（原地补字段）。
                 * @param[in] id   要打出的牌实例 id。
                 * @param[in] opts 该组全部合法动作；调用前保证非空。
                 * @return 补全 `instance_id` 与单元素 `targets` 的决策。
                 * @pre   `opts` 非空。
                 * @post 不改变任何状态。
                 */
                static DecisionChoice pick_single(
                    const AiView &view, DecisionChoice out, const std::string &id,
                    const std::vector<LegalAction> &opts);

                /**
                 * @brief  多目标动作：填入首卡实例 id 与完整目标集合。
                 * @param[in] out     已部分填好的决策（原地补字段）。
                 * @param[in] id      要打出的牌实例 id。
                 * @param[in] targets 引擎认可的完整目标集合。
                 * @return 补全 `instance_id` 与 `targets` 的决策。
                 * @post 不改变任何状态。
                 */
                static DecisionChoice pick_all(
                    DecisionChoice out, const std::string &id,
                    const std::vector<std::string> &targets);

                /**
                 * @brief 借刀杀人：在合法 {持武器者, 受害者} 对里选集火对象
                 *        （先按阵营优先度、再体力最低；同档同血取列表序），
                 *        整对作为目标传回。
                 * @param[in] view 观察（供目标优先级与体力比较）。
                 * @param[in] out  已部分填好的决策（原地补字段）。
                 * @param[in] id   要打出的牌实例 id。
                 * @param[in] opts 借刀合法动作集合；调用前保证非空且均为双目标。
                 * @return 补全 `instance_id` 与双元素 `targets` 的决策。
                 * @pre   `opts` 非空且每项 `targets.size() == 2`。
                 * @post 不改变任何状态。
                 * @note 目标是双元素对，不能走按 targets.front() 选目标的
                 *       单目标路径（那会把持武器者本身当目标）。
                 */
                static DecisionChoice pick_borrowed_sword(
                    const AiView &view, DecisionChoice out, const std::string &id,
                    const std::vector<LegalAction> &opts);

                /**
                 * @brief  查卡牌目录定义。
                 * @param[in] req    决策请求（`req.catalog` 可为空）。
                 * @param[in] def_id 卡牌定义 id。
                 * @return 命中返回定义指针；目录缺失或未命中返回 `nullptr`。
                 * @post 不改变任何状态；返回指针生命周期覆盖 `req`。
                 */
                static const card::CardDef *find_def(
                    const DecisionRequest &req, const std::string &def_id);

                /**
                 * @brief req.legal 中是否存在真杀动作（手牌有真杀）。
                 * @param[in] req 出牌类决策请求（`legal` 为合法动作集）。
                 * @return 任一动作的卡定义为「杀」时 true；目录缺失/未命中不算。
                 * @post 不改变任何状态。
                 * @note 与两张当杀 pair 的「手牌有真杀」同口径：有真杀时虚拟杀
                 *       不作优先，单张转化动作也不参与分组（避免烧牌与非法选择）。
                 */
                static bool has_real_sha(const DecisionRequest &req);

                /**
                 * @brief 单牌价值：弃牌排序「先弃最低价值」与选牌「取最高价值」
                 *        共用的估价。
                 * @param[in] req 决策请求（提供卡牌目录）。
                 * @param[in] c   待估值的牌。
                 * @return 牌价值；隐藏手牌占位取期望常量，目录缺失回 0。
                 * @post 不改变任何状态。
                 * @note 空 def_id 为隐藏手牌占位槽（身份不可知），按期望常量估值，
                 *       不得据真实牌面排序；目录缺失或 def 未命中时回 0。
                 */
                static int card_value_of(
                    const DecisionRequest &req, const card::Card &c);

                /**
                 * @brief  取观察中某角色的体力。
                 * @param[in] view 观察。
                 * @param[in] id   玩家 id。
                 * @return 命中角色的体力；不在观察中返回 0。
                 * @post 不改变任何状态。
                 */
                static int hp_of(const AiView &view, const std::string &id);

                // ── 身份局阵营判定 ──────────────────────────────────────────

                /**
                 * @brief 同阵营（对称）：主公/忠臣互认，反贼互认，内奸无友。
                 * @param[in] self  决策者角色。
                 * @param[in] other 待判角色。
                 * @return true = 同阵营；任一方 None 或内奸恒 false。
                 * @post 不改变任何状态。
                 */
                static bool is_friend(Role self, Role other);

                /**
                 * @brief 敌对（非对称）：主公/忠臣视反贼与内奸为敌，反贼视
                 *        主公与忠臣为敌，内奸只视反贼为敌（需借主公制衡反贼）。
                 * @param[in] self  决策者角色。
                 * @param[in] other 待判角色。
                 * @return true = 敌对；None 恒 false。
                 * @post 不改变任何状态。
                 */
                static bool is_enemy(Role self, Role other);

                /**
                 * @brief 是否仍有存活反贼（内奸保主制衡的开关）。
                 * @param[in] view 观察（`others` 只含存活者）。
                 * @return true = 自己为反贼或场上尚有反贼。
                 * @post 不改变任何状态。
                 * @note 观察的 others 只含存活者，已阵亡者不在此列。
                 */
                static bool any_rebel_alive(const AiView &view);

                /**
                 * @brief 是否仍有存活忠臣（内奸清场相位的对称判据）。
                 * @param[in] view 观察（`others` 只含存活者）。
                 * @return true = 自己为忠臣或场上尚有忠臣。
                 * @post 不改变任何状态。
                 * @note 观察的 others 只含存活者，已阵亡者不在此列。
                 */
                static bool any_loyalist_alive(const AiView &view);

                /**
                 * @brief 内奸是否仍须保主：场上还有任一派系（反贼或忠臣）
                 *        存活，主公尚非最后一名非内奸。
                 * @param[in] view 观察。
                 * @return true = 尚有反贼或忠臣存活。
                 * @post 不改变任何状态。
                 * @note 只看阵容存在性，与决策者自身角色无关；供保护者判定
                 *       使用，保证「内奸候选是否保主」由阵容而非视角决定。
                 */
                static bool traitor_keeps_lord(const AiView &view);

                /**
                 * @brief 内奸的落刀相位开关：无反贼且无忠臣存活时，主公已是
                 *        最后一名非内奸，杀主公即内奸独胜；在此之前内奸必须保
                 *        主：借主公制衡反贼，并先清光忠臣。
                 * @param[in] view 观察。
                 * @return true = 身份局内奸且已可落刀。
                 * @post 不改变任何状态。
                 * @note 只看阵容存在性，不看体力/手牌，纯函数可回放；乱斗与
                 *       非内奸角色恒 false，乱斗分支因此逐字节不变。
                 */
                static bool traitor_may_kill_lord(const AiView &view);

                /**
                 * @brief 是否应保护/避让该目标：自己、同阵营友方；内奸在反贼
                 *        或忠臣尚存时额外包含主公（主公成为最后一名非内奸前不
                 *        落刀）。
                 * @param[in] view 观察。
                 * @param[in] id   待判目标 id。
                 * @return true = 应保护/避让。
                 * @post 不改变任何状态。
                 * @note 乱斗/无角色只保护自己，其余恒 false。
                 */
                static bool protects(const AiView &view, const std::string &id);

                /**
                 * @brief 避让档：应保护目标记 1，其余记 0（小者优先）。
                 * @param[in] view 观察。
                 * @param[in] id   待判目标 id。
                 * @return 1 = 应保护；0 = 可攻击。
                 * @post 不改变任何状态。
                 */
                static int avoid_rank(const AiView &view, const std::string &id);

                /**
                 * @brief 复刻结算侧的座位序（从 start 起环绕），仅用观察中的
                 *        自身与其他存活角色的座位号。
                 * @param[in] view  观察。
                 * @param[in] start 窗口起始角色 id。
                 * @return 从 `start` 起按座位环绕的角色 id 序。
                 * @post 不改变任何状态。
                 * @note 与 EntityManager::order_from 同口径（按座位稳定排序后
                 *       旋转到 start）；start 不在观察中时保持原序。
                 */
                static std::vector<std::string> window_order(
                    const AiView &view, const std::string &start);

                /**
                 * @brief candidate 是否是 target 的保护者：本人、同阵营友方，
                 *        或（尚有其他派系存活时）保主的内奸。
                 * @param[in] view      观察。
                 * @param[in] candidate 待判角色 id。
                 * @param[in] target    被保护目标 id。
                 * @return true = `candidate` 保护 `target`。
                 * @post 不改变任何状态。
                 * @note 内奸候选是否保主只看阵容存在性，不能按决策者角色判定，
                 *       否则主公视角会误判内奸为首位保护者而拒绝自己的无懈。
                 */
                static bool is_protector_of(
                    const AiView &view, const std::string &candidate,
                    const std::string &target);

                /**
                 * @brief 决策者是否是 target 在窗口序中的首位保护者：同一轮
                 *        至多一名保护者出手；首位保护者持多张无懈时仍会被逐轮
                 *        重问（已知残差）。
                 * @param[in] view   观察。
                 * @param[in] target 被保护目标 id。
                 * @param[in] start  窗口起始角色 id。
                 * @return true = 决策者是窗口序首位保护者。
                 * @post 不改变任何状态。
                 */
                static bool is_first_protector(
                    const AiView &view, const std::string &target,
                    const std::string &start);

                // ── 有害出牌组过滤 ────────────────────────────────────────

                /**
                 * @brief 是否是对单一目标有害的效果：伤害/决斗/弃牌/顺牌与
                 *        延时锦囊；自益（摸牌/回复）、群体与借刀不算。
                 * @param[in] def 卡牌定义。
                 * @return true = 对单一目标有害。
                 * @post 不改变任何状态。
                 */
                static bool is_harmful_def(const card::CardDef &def);

                /**
                 * @brief 组内动作是否全部只打应保护目标：身份局下用于整组跳过
                 *        （无目标动作与对自己使用的牌不参与过滤，直接判否）。
                 * @param[in] req  出牌类决策请求（提供观察与身份）。
                 * @param[in] opts 同一张牌的合法动作组。
                 * @return true = 该组全部动作都只打应保护目标。
                 * @post 不改变任何状态。
                 * @note 乱斗/无角色恒 false，保证乱斗逐字节不变。
                 */
                static bool group_avoids_all_targets(
                    const DecisionRequest &req, const std::vector<LegalAction> &opts);

                /**
                 * @brief 阵营目标优先度：身份局按决策者阵营给敌对目标降档，
                 *        数值小者优先；乱斗或无角色时全目标恒 0（退回最低体力）。
                 * @param[in] view 观察（含自身角色与其他角色）。
                 * @param[in] id   待评估目标。
                 * @return 优先度：主公/忠臣视反贼与内奸同档最优先、其余后置；
                 *         反贼视主公最优先、忠臣次之、其余最后；内奸或自身角色
                 *         缺失一律 0。
                 * @post 不改变任何状态。
                 * @note 目标角色未知时按非敌意处理（主公阵营后置、反贼归最后档）。
                 */
                static int target_priority(const AiView &view, const std::string &id);

                /**
                 * @brief 集火：在单目标动作里先按阵营优先度、再按体力最低者
                 *        （同档同血取列表序）。
                 * @param[in] view 观察。
                 * @param[in] opts 单目标动作集合；调用前保证非空。
                 * @return 选中的目标 id。
                 * @pre   `opts` 非空且每项 `targets` 非空。
                 * @post 不改变任何状态。
                 * @note 乱斗或无角色时优先度恒 0，比较器退化为原「体力最低」。
                 */
                static std::string lowest_hp_action(
                    const AiView &view, const std::vector<LegalAction> &opts);

                /**
                 * @brief 单目标选择：身份局先避开应保护目标（同阵营友方与
                 *        内奸保主），再按阵营优先度、体力最低（同档同血取列表序）。
                 * @param[in] view 观察。
                 * @param[in] opts 单目标动作集合；调用前保证非空。
                 * @return 选中的目标 id。
                 * @pre   `opts` 非空且每项 `targets` 非空。
                 * @post 不改变任何状态。
                 * @note 乱斗/无角色时避让档恒 0，比较器退化为原「阵营优先度 +
                 *       最低体力」；丈八 pair 的组内目标不经过本函数，保持既有
                 *       集火/最低血口径。
                 */
                static std::string best_single_target(
                    const AiView &view, const std::vector<LegalAction> &opts);
            };
        }
    }
}

#endif  // INCLUDE_TKW_GAME_DECIDER_BASE_HPP
