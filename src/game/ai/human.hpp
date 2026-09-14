/**
 * @file   human.hpp
 * @brief  真人决策接入：从输入流读取玩家选择，供 CLI 交互对局使用。
 * @details 只读契约不变：`HumanDecider` 只消费输入流并返回选择，不触碰对局状态；
 *          读取输入是决策源自身的副作用，串行 REPL 下与命令行解析无双读。
 *          候选按 1 起编号；非法输入重提示；窗口内输入 `?` / `help` 打印用法、
 *          `card <序号>` 查看候选牌面；EOF 打印提示后按放弃返回。
 * @ingroup tkw_game_ai
 */

#ifndef INCLUDE_TKW_GAME_HUMAN_HPP
#define INCLUDE_TKW_GAME_HUMAN_HPP

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <istream>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "card/card.hpp"
#include "card/catalog.hpp"
#include "card/def.hpp"
#include "game/ai/decider.hpp"
#include "game/ai/simple.hpp"
#include "game/core/decision.hpp"
#include "util/types.hpp"

namespace tkw
{
    namespace game
    {
        namespace ai
        {
            /**
             * @class HumanDecider
             * @brief 交互式决策：渲染编号候选并读取一行回复。
             * @note 构造注入输入/输出流，单测可脚本化。读取失败（EOF）按放弃
             *       处理并立即返回，绝不重提示，避免关闭的管道陷入死循环。
             * @warning 只读对局状态：只输出提示并返回选择；读取输入是自身副作用。
             */
            class HumanDecider : public Decider
            {
            public:
                /**
                 * @brief  以指定输入/输出流构造。
                 * @param[in] in  输入流，用于读取玩家回复；生命周期须覆盖本对象。
                 * @param[in] out 输出流，用于渲染提示与候选；生命周期须覆盖本对象。
                 */
                HumanDecider(std::istream &in, std::ostream &out) :
                    m_in(in), m_out(out)
                {
                }

                /**
                 * @brief  按决策类别渲染窗口并读取玩家选择。
                 * @param[in] request 决策请求。
                 * @return 玩家选择；EOF 或放弃时返回空选择。
                 * @post 不修改对局状态；仅向 `m_out` 写提示并消费 `m_in`。
                 */
                DecisionChoice decide(const DecisionRequest &request) override;

            private:
                std::istream &m_in;
                std::ostream &m_out;

                /**
                 * @brief  读一行并去掉行尾 CR；EOF/读失败返回 false。
                 * @param[out] line 读到的行（已去 `\r`）。
                 * @return 读取成功返回 true，EOF/失败返回 false。
                 * @retval true  成功读到一行。
                 * @retval false 输入流结束或读取失败。
                 * @post 失败时 `line` 内容不保证。
                 */
                bool read_line(std::string &line);

                /**
                 * @brief  按空白切分一行；空行返回空 token 列表。
                 * @param[in] line 待切分的行。
                 * @return 以空白分隔的 token 列表（空行时为空）。
                 * @post 不改变任何状态。
                 */
                static std::vector<std::string> tokenize(const std::string &line);

                /**
                 * @brief 读取一行并切词：EOF/读失败返回 None；空行打印换行后
                 *        返回 Some(空列表)。
                 * @return None 表示输入结束，调用点返回默认选择；Some 表示
                 *         一行的 token，空列表表示空行（本函数已打印 `\n`）。
                 * @note EOF 单点打印「输入已结束，按放弃处理」后返回 None，
                 *       覆盖全部决策窗口；各窗口按自身语义把空选择交给引擎。
                 *       非法输入的重提示与序号解析留在调用点，各决策保留其
                 *       特有返回语义。
                 */
                Option<std::vector<std::string>> read_tokens();

                /**
                 * @brief  该行是否为窗口内帮助请求（`?` / `help`）。
                 * @param[in] tokens 一行切分后的 token 列表。
                 * @return 单个 token 且为 `?` 或 `help` 时 true。
                 * @post 不改变任何状态。
                 */
                static bool is_help(const std::vector<std::string> &tokens)
                {
                    return tokens.size() == 1 &&
                           (tokens[0] == "?" || tokens[0] == "help");
                }

                /** @brief 候选窗口 Prompt 尾注：`?` 看用法、`card <序号>` 看候选牌面。 */
                static constexpr const char *kCardHint =
                    "? 看用法，card <序号> 看牌面";

                /** @brief 无候选窗口 Prompt 尾注：只支持 `?` 看用法。 */
                static constexpr const char *kHelpHint = "? 看用法";

                /**
                 * @brief 打印当前决策窗口的输入用法（`?`/`help` 触发）。
                 * @param[in] tip 用法正文，不含「用法：」前缀。
                 * @note 只打印窗口语法，不打印任何卡牌效果文案（那由
                 *       `card <序号>` 负责）；不改变窗口返回契约。
                 */
                void print_help(const std::string &tip)
                {
                    m_out << "用法：" << tip << "\n";
                }

                /**
                 * @brief  解析 1 基序号并校验落在 [1, count]；失败返回 false。
                 * @param[in]  token 序号 token。
                 * @param[in]  count 候选总数（合法上界）。
                 * @param[out] out   解析成功时写入 1 基序号。
                 * @return 解析且范围合法时 true。
                 * @retval true  写入 `out`。
                 * @retval false 非整数、含多余字符或越界；`out` 不保证。
                 * @post 不改变对局状态。
                 */
                static bool parse_index(
                    const std::string &token, std::size_t count, int &out);

                /**
                 * @brief  把某个区域渲染成「卡名/卡名」；空区域回落「无」。
                 * @param[in] req  决策请求（提供卡牌目录）。
                 * @param[in] zone 待渲染的牌区。
                 * @return 卡名以 `/` 连接；空区域返回「无」。
                 * @post 不改变任何状态。
                 */
                static std::string zone_names(
                    const DecisionRequest &req,
                    const std::vector<card::Card> &zone);

                /**
                 * @brief 打印决策者视角的局面摘要：己方体力/完整手牌/装备/判定，
                 *        其余角色逐行给体力/手牌数/装备/距离。
                 * @param[in] req 决策请求（渲染其 `view`）。
                 * @post 只写 `m_out`，不改变对局状态与候选。
                 * @note 严格渲染 req.view 的可见性边界：己方手牌展开牌名，其他
                 *       角色手牌只出数量，绝不展开牌面内容。
                 * @note 纯展示，不改变候选与输入语法；每次重提示都会重绘。
                 */
                void print_view(const DecisionRequest &req);

                /**
                 * @brief 打印单张候选牌的效果文案（数据源 CardDef.text）。
                 * @param[in] req    决策请求（提供卡牌目录）。
                 * @param[in] def_id 卡牌定义 id；目录未收录回落 id 展示。
                 * @post 只写 `m_out`，不改变对局状态。
                 * @note 文案缺失时打印「（无说明）」占位；只读查询，不改状态。
                 */
                void print_card_text(
                    const DecisionRequest &req, const std::string &def_id);

                /** @brief 对手手牌候选项在候选列表中的遮挡占位文本。 */
                static constexpr const char *kHiddenHandPlaceholder = "（未知手牌）";

                /** @brief 查询对手手牌候选项牌文时的遮挡提示。 */
                static constexpr const char *kHiddenHandCardHint =
                    "（未知手牌，无法查看）";

                /**
                 * @brief 候选是否属于对手手牌、需向决策者遮挡内容。
                 * @param[in] req   当前决策请求。
                 * @param[in] index 候选的 0 基下标。
                 * @return 仅 PickCard 的手牌候选返回 true；缺少分区标签时防御性
                 *         返回 true（平行数组缺口不得导致漏遮）。
                 * @note 装备区/判定区为明置信息，其余决策类别的候选均为决策者
                 *       自己可见的牌，一律返回 false。此谓词是候选遮挡的唯一判据。
                 */
                static bool is_hidden_option(
                    const DecisionRequest &req, std::size_t index);

                /**
                 * @brief 处理窗口内 `card <序号>`：打印该候选牌的效果文案。
                 * @tparam Candidates 候选容器类型。
                 * @tparam DefIdOf    从候选元素取卡牌定义 id 的一元函数。
                 * @param[in] req        当前决策请求。
                 * @param[in] token      序号 token。
                 * @param[in] candidates 候选容器，序号与窗口打印的 1 基编号一致。
                 * @param[in] def_id_of  候选 → 定义 id 投影。
                 * @note 序号非法时打印统一范围提示；合法时打印该牌效果文案，
                 *       但对手手牌候选只打印遮挡提示，不泄漏牌名与效果文案。
                 *       各路径都不改变候选与窗口状态，调用点继续循环。
                 */
                template <typename Candidates, typename DefIdOf>
                void show_card_or_hint(
                    const DecisionRequest &req, const std::string &token,
                    const Candidates &candidates, DefIdOf def_id_of)
                {
                    int index = 0;
                    if (!parse_index(token, candidates.size(), index))
                    {
                        print_invalid(index_hint(candidates.size()));
                        return;
                    }
                    if (is_hidden_option(req, static_cast<std::size_t>(index - 1)))
                    {
                        m_out << kHiddenHandCardHint << "\n";
                        return;
                    }
                    print_card_text(
                        req, def_id_of(
                                 candidates[static_cast<std::size_t>(index - 1)]));
                }

                /**
                 * @brief  目标牌来源分区标签；非选牌决策返回空串。
                 * @param[in] zone 来源分区。
                 * @return 手/装/判对应标签；其余返回空串。
                 * @post 不改变任何状态。
                 */
                static const char *zone_tag(card::Zone zone);

                /**
                 * @brief 打印 1 基编号的牌候选列表。
                 * @param[in] req     决策请求（提供目录与分区标签）。
                 * @param[in] options 候选牌列表。
                 * @post 只写 `m_out`，不改变对局状态。
                 * @note 候选带来源分区标签（选目标牌）时在牌名前标注
                 *       `[手]/[装]/[判]`，其余决策无标签保持原样。选目标牌的
                 *       手牌候选只打印分区标签与遮挡占位，不显示牌名与实例号；
                 *       装备/判定区明置，照常显示。
                 */
                void print_options(
                    const DecisionRequest &req,
                    const std::vector<card::Card> &options);

                /**
                 * @brief  打印重提示分隔行与具体原因，并附 `?` 出口，保留「输入无效」标识。
                 * @param[in] reason 具体原因文案。
                 * @post 只写 `m_out`，不改变对局状态。
                 */
                void print_invalid(const std::string &reason)
                {
                    m_out << "\n输入无效：" << reason << "（" << kHelpHint << "）\n";
                }

                /**
                 * @brief  1 基序号的合法范围提示文本。
                 * @param[in] count 候选总数。
                 * @return 「序号需为 1-count 之间的整数。」文案。
                 * @post 不改变任何状态。
                 */
                static std::string index_hint(std::size_t count)
                {
                    return "序号需为 1-" + std::to_string(count) + " 之间的整数。";
                }

                /**
                 * @brief 响应窗口标题：有来源牌时给来源与伤害后果，否则回落
                 *        「需打出闪/杀」旧口径（兼容无上下文调用）。
                 * @param[in] req  响应类决策请求。
                 * @param[in] pair 是否两张手牌当杀窗口（丈八蛇矛）。
                 * @return 窗口标题文案。
                 * @post 不改变任何状态。
                 */
                std::string response_title(const DecisionRequest &req, bool pair);

                /**
                 * @brief  亮牌窗口标题：按来源结算分别渲染五谷丰登/麒麟弓/火攻。
                 * @param[in] req 选亮牌类决策请求。
                 * @return 窗口标题文案。
                 * @post 不改变任何状态。
                 */
                static std::string reveal_title(const DecisionRequest &req);

                /**
                 * @brief  装备能力一句话效果（只读展示，不打印卡牌 text）。
                 * @param[in] ability 装备能力。
                 * @return 能力效果文案；未知能力回落「装备能力」。
                 * @post 不改变任何状态。
                 */
                static const char *ability_hint(card::Ability ability);

                /**
                 * @brief 无懈窗口提示标题：锦囊使用者（判定窗口使用者不可考 →
                 *        延时判定标注）与目标集合；有锦囊名时一并渲染。
                 * @param[in] req 无懈类决策请求。
                 * @return 窗口标题文案。
                 * @post 不改变任何状态。
                 */
                static std::string counter_title(const DecisionRequest &req);

                static const char *reason_text(DiscardReason reason);

                /**
                 * @brief  从装备区反查携带该能力的装备名；查不到回落「装备能力」。
                 * @param[in] req 触发类决策请求（含观察装备与目标能力）。
                 * @return 装备名或「装备能力」。
                 * @post 不改变任何状态。
                 */
                static std::string ability_name(const DecisionRequest &req);

                /**
                 * @brief  武将触发技一句话效果（只读展示；非触发技回落通用文案）。
                 * @param[in] skill 武将技能。
                 * @return 技能效果文案或「武将技能」。
                 * @post 不改变任何状态。
                 */
                static const char *hero_skill_hint(hero::HeroSkill skill);

                /**
                 * @brief  触发窗技能名：武将触发技取技能中文名，装备能力取装备名。
                 * @param[in] req 触发类决策请求。
                 * @return 技能名或装备名。
                 * @post 不改变任何状态。
                 */
                static std::string trigger_name(const DecisionRequest &req);

                /**
                 * @brief  触发窗一句话提示：按装备能力/武将触发技分流。
                 * @param[in] req 触发类决策请求。
                 * @return 技能/能力效果文案。
                 * @post 不改变任何状态。
                 */
                static std::string trigger_hint(const DecisionRequest &req);

                /**
                 * @brief 借刀杀人候选的受害者是否为决策者本人。
                 * @param[in]  req    当前出牌决策请求。
                 * @param[in]  act    待判定的合法动作。
                 * @param[out] holder 出参：命中时写入持武器者 id（targets[0]）。
                 * @return 是则 true；非借刀/非双目标/受害者非本人则 false。
                 * @note 只读，不改变候选与输入；用于出牌窗口的后果提示。
                 */
                static bool is_self_target_borrowed_sword(
                    const DecisionRequest &req, const LegalAction &act,
                    std::string &holder);

                /**
                 * @brief  出牌窗口：渲染合法动作并读取 `play <序号>` / `pass`。
                 * @param[in] req 出牌类决策请求（合法动作在 `legal`）。
                 * @return 选中的动作；`None` = 结束出牌阶段或 EOF。
                 * @post 不改变对局状态；仅读写流。
                 */
                DecisionChoice decide_play(const DecisionRequest &req);

                /**
                 * @brief  单张手牌选择（响应/救桃/无懈）：`play <序号>` 或 `pass`。
                 * @param[in] req   决策请求（候选在 `options`）。
                 * @param[in] title 窗口标题，由调用点按类别给定。
                 * @return 选中的牌；`None` = 放弃或 EOF。
                 * @retval instance_id 玩家选择的手牌实例 id。
                 * @retval empty 放弃（含 EOF）。
                 * @post 不改变对局状态；仅读写流。
                 */
                DecisionChoice decide_choose_id(
                    const DecisionRequest &req, const std::string &title);

                /**
                 * @brief 杀响应窗口、两张手牌当杀（丈八蛇矛）：手牌编号渲染，
                 *        读 `play <序号> + <序号>`；本模式下无真响应牌候选。
                 * @param[in] req 响应类决策请求（`legal` 为两张当杀 pair 候选）。
                 * @return 选中的两张手牌；`None` = 放弃或 EOF。
                 * @retval instance_id 第一张手牌实例 id。
                 * @retval empty 放弃（含 EOF）。
                 * @post 不改变对局状态；仅读写流。
                 * @note 读取失败（EOF）按放弃处理并立即返回，绝不重提示。
                 */
                DecisionChoice decide_response_pair(const DecisionRequest &req);

                /**
                 * @brief 从牌池选一张：`pick <序号>`（allow_pass 时或 `pass`）。
                 * @param[in] req        选牌类决策请求（候选在 `options`）。
                 * @param[in] title      窗口标题（五谷/麒麟/目标区域，由调用点按来源定）。
                 * @param[in] allow_pass 拆/顺允许放弃（None 由引擎按非法选择处理）；
                 *        五谷为强制选择，不提供 pass，输入 pass 视为非法重提示。
                 * @return 选中的候选下标；`None` = 放弃或 EOF。
                 * @retval option_index 选中的 0 基候选下标。
                 * @retval empty 放弃（含 EOF 与强制选择下的非法放弃）。
                 * @post 不改变对局状态；仅读写流。
                 * @note EOF 在两种模式下都返回空选择，由引擎给出诊断。
                 */
                DecisionChoice decide_pick(
                    const DecisionRequest &req, const std::string &title,
                    bool allow_pass);

                /**
                 * @brief  弃牌窗口：渲染手牌并读取 `discard <序号>...`（雌雄可选 `pass`）。
                 * @param[in] req 弃牌类决策请求（候选在 `options`，数量 = `count`）。
                 * @return 要弃置的牌实例 id 列表；空列表表示放弃或 EOF。
                 * @post 不改变对局状态；仅读写流。
                 */
                DecisionChoice decide_discard(const DecisionRequest &req);

                /**
                 * @brief  触发窗口：渲染技能名并读取 `y` / `n`。
                 * @param[in] req 触发类决策请求。
                 * @return `accepted` 置位的决策；EOF 时为不发动。
                 * @post 不改变对局状态；仅读写流。
                 */
                DecisionChoice decide_trigger(const DecisionRequest &req);
            };

            /**
             * @class HumanAI
             * @brief 引擎可用的真人决策源：HumanDecider 经适配器接入。
             * @note 输入/输出流由调用方持有，其生命周期须覆盖本对象。
             */
            class HumanAI : private HumanDecider, public RequestDecisionSource
            {
            public:
                /**
                 * @brief  以输入/输出流构造，并把自身接入适配器。
                 * @param[in] in  输入流；生命周期须覆盖本对象。
                 * @param[in] out 输出流；生命周期须覆盖本对象。
                 */
                HumanAI(std::istream &in, std::ostream &out) :
                    HumanDecider(in, out),
                    RequestDecisionSource(static_cast<HumanDecider &>(*this))
                {
                }
            };

            /**
             * @class RoutedAI
             * @brief 按决策者 id 路由：命中真人座位走交互输入，其余回落注入的 AI 决策源。
             * @note 每个回调携带的 actor 即该决策的发起者（出牌为回合角色，响应/
             *       救桃/无懈/弃牌/选牌为当事人）；路由只按 id 查表，不改接缝。
             *       回落决策源构造注入（难度档同源注入点）；三参构造回落贪心档。
             * @warning 只读：全部回调只读 `ReadOnlyContext`，不修改对局状态。
             */
            class RoutedAI : public DecisionSource
            {
            public:
                /**
                 * @brief  以真人座位集合构造，回落决策源固定为贪心档。
                 * @param[in] humans 真人座位 id 集合。
                 * @param[in] in     真人输入流；生命周期须覆盖本对象。
                 * @param[in] out    真人输出流；生命周期须覆盖本对象。
                 */
                RoutedAI(
                    const std::vector<std::string> &humans, std::istream &in,
                    std::ostream &out) :
                    RoutedAI(humans, in, out, std::make_unique<SimpleAI>())
                {
                }

                /**
                 * @brief  以真人座位集合与回落决策源构造。
                 * @param[in] humans   真人座位 id 集合。
                 * @param[in] in       真人输入流；生命周期须覆盖本对象。
                 * @param[in] out      真人输出流；生命周期须覆盖本对象。
                 * @param[in] fallback 非真人座位的回落决策源；本对象接管其所有权。
                 */
                RoutedAI(
                    const std::vector<std::string> &humans, std::istream &in,
                    std::ostream &out, std::unique_ptr<DecisionSource> fallback) :
                    m_fallback(std::move(fallback))
                {
                    for (const auto &id : humans)
                        m_humans.emplace(id, std::make_unique<HumanAI>(in, out));
                }

                /**
                 * @brief  按 `entity` 路由到对应决策源，转发响应窗口回调。
                 * @param[in] ctx    只读容器视图。
                 * @param[in] entity 被询问的实体 id。
                 * @param[in] kind   需要的响应牌类别。
                 * @param[in] prompt 来源牌/使用者/伤害量。
                 * @return 要打出的响应动作；`None` = 不响应。
                 * @post 本接口不改变任何状态。
                 */
                Option<PlayAction> play_response(
                    const ReadOnlyContext &ctx, const std::string &entity,
                    card::ResponseKind kind, const ResponsePrompt &prompt) override
                {
                    return route(entity).play_response(ctx, entity, kind, prompt);
                }

                /**
                 * @brief  按 `source` 路由，转发目标区域选牌回调。
                 * @param[in] ctx    只读容器视图。
                 * @param[in] source 决策者 id。
                 * @param[in] target 被选牌的目标 id。
                 * @param[in] scope  可选取域（手/装备/判定）。
                 * @return 选中的目标牌；`None` = 放弃或下标越界。
                 * @post 本接口不改变任何状态。
                 */
                Option<TargetPick> pick_card_from_target(
                    const ReadOnlyContext &ctx, const std::string &source,
                    const std::string &target, PickCardScope scope) override
                {
                    return route(source).pick_card_from_target(
                        ctx, source, target, scope);
                }

                /**
                 * @brief  按 `turn.player` 路由，转发出牌阶段回调。
                 * @param[in] ctx  只读容器视图。
                 * @param[in] turn 当前回合上下文（决策者 = `turn.player`）。
                 * @return 要执行的动作；`None` = 结束出牌阶段。
                 * @post 本接口不改变任何状态。
                 */
                Option<PlayAction> choose_play(
                    const ReadOnlyContext &ctx, const TurnContext &turn) override
                {
                    return route(turn.player).choose_play(ctx, turn);
                }

                /**
                 * @brief  按 `player` 路由，转发弃牌回调。
                 * @param[in] ctx    只读容器视图。
                 * @param[in] player 决策者 id。
                 * @param[in] count  需弃牌数。
                 * @param[in] reason 弃牌原因。
                 * @return 要弃置的牌实例 id 列表。
                 * @post 本接口不改变任何状态。
                 */
                std::vector<std::string> choose_discards(
                    const ReadOnlyContext &ctx, const std::string &player, int count,
                    DiscardReason reason) override
                {
                    return route(player).choose_discards(ctx, player, count, reason);
                }

                /**
                 * @brief  按 `player` 路由，转发亮牌选牌回调。
                 * @param[in] ctx     只读容器视图。
                 * @param[in] player  决策者 id。
                 * @param[in] options 可选的亮牌。
                 * @param[in] source  亮牌来源结算。
                 * @return 选中的牌；`None` = 放弃或下标越界。
                 * @post 本接口不改变任何状态。
                 */
                Option<card::Card> pick_from_revealed(
                    const ReadOnlyContext &ctx, const std::string &player,
                    const std::vector<card::Card> &options,
                    RevealSource source) override
                {
                    return route(player).pick_from_revealed(
                        ctx, player, options, source);
                }

                /**
                 * @brief  按 `saver` 路由，转发濒死救场回调。
                 * @param[in] ctx   只读容器视图。
                 * @param[in] saver 救援者 id。
                 * @param[in] dying 濒死者 id。
                 * @return 打出的救场牌；`None` = 不救。
                 * @post 本接口不改变任何状态。
                 */
                Option<std::string> play_peach(
                    const ReadOnlyContext &ctx, const std::string &saver,
                    const std::string &dying) override
                {
                    return route(saver).play_peach(ctx, saver, dying);
                }

                /**
                 * @brief  按 `player` 路由，转发无懈窗口回调。
                 * @param[in] ctx            只读容器视图。
                 * @param[in] player         被询问的实体 id。
                 * @param[in] trick_user     锦囊使用者 id。
                 * @param[in] trick_targets  锦囊目标集合。
                 * @param[in] trick_def_id   被无懈的锦囊 def id。
                 * @param[in] counter_played 本窗已打出的无懈张数。
                 * @return 打出的无懈；`None` = 不出。
                 * @post 本接口不改变任何状态。
                 */
                Option<std::string> play_counter(
                    const ReadOnlyContext &ctx, const std::string &player,
                    const std::string &trick_user,
                    const std::vector<std::string> &trick_targets,
                    const std::string &trick_def_id,
                    int counter_played = 0) override
                {
                    return route(player).play_counter(
                        ctx, player, trick_user, trick_targets, trick_def_id,
                        counter_played);
                }

                /**
                 * @brief  按 `player` 路由，转发装备能力触发回调。
                 * @param[in] ctx     只读容器视图。
                 * @param[in] player  决策者 id。
                 * @param[in] ability 待触发的装备能力。
                 * @return true = 发动。
                 * @post 本接口不改变任何状态。
                 */
                bool trigger_effect(
                    const ReadOnlyContext &ctx, const std::string &player,
                    card::Ability ability) override
                {
                    return route(player).trigger_effect(ctx, player, ability);
                }

                /**
                 * @brief  按 `player` 路由，转发武将触发技回调。
                 * @param[in] ctx    只读容器视图。
                 * @param[in] player 决策者 id。
                 * @param[in] skill  待触发的武将技能。
                 * @param[in] cause  触发来源 id（空 = 无来源）。
                 * @return true = 发动。
                 * @post 本接口不改变任何状态。
                 */
                bool trigger_hero_skill(
                    const ReadOnlyContext &ctx, const std::string &player,
                    hero::HeroSkill skill, const std::string &cause) override
                {
                    return route(player).trigger_hero_skill(
                        ctx, player, skill, cause);
                }

            private:
                std::unique_ptr<DecisionSource> m_fallback; /**< 非真人座位的回落决策源。 */
                std::map<std::string, std::unique_ptr<HumanAI>> m_humans; /**< 真人座位 → 交互决策源。 */

                /**
                 * @brief  查表取得 actor 对应的决策源。
                 * @param[in] actor 决策发起者 id。
                 * @return 命中真人座位返回其 `HumanAI`，否则返回 `m_fallback`。
                 * @post 不改变任何状态；返回引用生命周期覆盖本对象。
                 */
                DecisionSource &route(const std::string &actor)
                {
                    const auto it = m_humans.find(actor);
                    if (it != m_humans.end())
                        return static_cast<DecisionSource &>(*it->second);
                    return *m_fallback;
                }
            };
        }
    }
}

#endif  // INCLUDE_TKW_GAME_HUMAN_HPP
