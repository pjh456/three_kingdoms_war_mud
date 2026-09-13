/**
 * @file human.hpp
 * @brief 真人决策接入：从输入流读取玩家选择，供 CLI 交互对局使用。
 * @note 只读契约不变：HumanDecider 只消费输入流并返回选择，不触碰对局状态；
 *       读取输入是决策源自身的副作用，串行 REPL 下与命令行解析无双读。
 *       候选按 1 起编号；非法输入重提示；窗口内输入 ? / help 打印用法、
 *       card <序号> 查看候选牌面；EOF 打印提示后按放弃返回。
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
             */
            class HumanDecider : public Decider
            {
            public:
                HumanDecider(std::istream &in, std::ostream &out) :
                    in_(in), out_(out)
                {
                }

                DecisionChoice decide(const DecisionRequest &request) override
                {
                    switch (request.kind)
                    {
                    case DecisionKind::Play:
                        return decide_play(request);
                    case DecisionKind::Response:
                        if (!request.legal.empty())
                            return decide_response_pair(request);
                        if (request.options.empty())
                        {
                            out_ << "无可用响应牌。\n";
                            return DecisionChoice{};
                        }
                        return decide_choose_id(
                            request, response_title(request, false));
                    case DecisionKind::Peach:
                        if (request.options.empty())
                        {
                            out_ << "无可用救场牌。\n";
                            return DecisionChoice{};
                        }
                        return decide_choose_id(
                            request,
                            "濒死救场（濒死者: " + request.dying + "）");
                    case DecisionKind::Counter:
                        if (request.options.empty())
                        {
                            out_ << "无可用无懈可击。\n";
                            return DecisionChoice{};
                        }
                        return decide_choose_id(request, counter_title(request));
                    case DecisionKind::Trigger:
                        return decide_trigger(request);
                    case DecisionKind::PickCard:
                        return decide_pick(request, "选择目标区域的牌", true);
                    case DecisionKind::PickRevealed:
                        return decide_pick(
                            request, reveal_title(request),
                            request.reveal_source ==
                                RevealSource::FireAttackDiscard);
                    case DecisionKind::Discard:
                        return decide_discard(request);
                    }
                    return DecisionChoice{};
                }

            private:
                std::istream &in_;
                std::ostream &out_;

                /** @brief 读一行并去掉行尾 CR；EOF/读失败返回 false。 */
                bool read_line(std::string &line)
                {
                    if (!std::getline(in_, line))
                        return false;
                    if (!line.empty() && line.back() == '\r')
                        line.pop_back();
                    return true;
                }

                /** @brief 按空白切分一行；空行返回空 token 列表。 */
                static std::vector<std::string> tokenize(const std::string &line)
                {
                    std::vector<std::string> out;
                    std::size_t i = 0;
                    while (i < line.size())
                    {
                        while (i < line.size() &&
                               std::isspace(static_cast<unsigned char>(line[i])))
                            ++i;
                        const std::size_t start = i;
                        while (i < line.size() &&
                               !std::isspace(static_cast<unsigned char>(line[i])))
                            ++i;
                        if (i > start)
                            out.push_back(line.substr(start, i - start));
                    }
                    return out;
                }

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
                Option<std::vector<std::string>> read_tokens()
                {
                    std::string line;
                    if (!read_line(line))
                    {
                        out_ << "\n输入已结束，按放弃处理。\n";
                        return Option<std::vector<std::string>>::None();
                    }

                    auto tokens = tokenize(line);
                    if (tokens.empty())
                        out_ << "\n";
                    return Option<std::vector<std::string>>::Some(std::move(tokens));
                }

                /** @brief 该行是否为窗口内帮助请求（? / help）。 */
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
                 * @param tip 用法正文，不含「用法：」前缀。
                 * @note 只打印窗口语法，不打印任何卡牌效果文案（那由
                 *       `card <序号>` 负责）；不改变窗口返回契约。
                 */
                void print_help(const std::string &tip)
                {
                    out_ << "用法：" << tip << "\n";
                }

                /** @brief 解析 1 基序号并校验落在 [1, count]；失败返回 false。 */
                static bool parse_index(
                    const std::string &token, std::size_t count, int &out)
                {
                    int value = 0;
                    const char *first = token.data();
                    const char *last = token.data() + token.size();
                    const auto result = std::from_chars(first, last, value);
                    if (result.ec != std::errc() || result.ptr != last)
                        return false;
                    if (value < 1 || value > static_cast<int>(count))
                        return false;
                    out = value;
                    return true;
                }

                /** @brief 把某个区域渲染成「卡名/卡名」；空区域回落「无」。 */
                static std::string zone_names(
                    const DecisionRequest &req,
                    const std::vector<card::Card> &zone)
                {
                    if (zone.empty())
                        return "无";
                    std::string out;
                    for (std::size_t i = 0; i < zone.size(); ++i)
                    {
                        if (i > 0)
                            out += "/";
                        out += card::display_name(req.catalog, zone[i].def_id);
                    }
                    return out;
                }

                /**
                 * @brief 打印决策者视角的局面摘要：己方体力/完整手牌/装备/判定，
                 *        其余角色逐行给体力/手牌数/装备/距离。
                 * @note 严格渲染 req.view 的可见性边界：己方手牌展开牌名，其他
                 *       角色手牌只出数量，绝不展开牌面内容。
                 * @note 纯展示，不改变候选与输入语法；每次重提示都会重绘。
                 */
                void print_view(const DecisionRequest &req)
                {
                    const auto &v = req.view;
                    out_ << "[" << v.self << "] 体力 " << v.self_hp << "/"
                         << v.self_max_hp << "  手牌 " << zone_names(req, v.hand)
                         << "  装备 " << zone_names(req, v.equip) << "  判定 "
                         << zone_names(req, v.judge) << "\n";
                    for (const auto &e : v.others)
                        out_ << e.id << " 体力 " << e.hp << "/" << e.max_hp
                             << "  手牌 " << e.hand_size << "  装备 "
                             << zone_names(req, e.equip) << "  距离 " << e.distance
                             << "\n";
                }

                /**
                 * @brief 打印单张候选牌的效果文案（数据源 CardDef.text）。
                 * @param def_id 卡牌定义 id；目录未收录回落 id 展示。
                 * @note 文案缺失时打印「（无说明）」占位；只读查询，不改状态。
                 */
                void print_card_text(
                    const DecisionRequest &req, const std::string &def_id)
                {
                    std::string text = "（无说明）";
                    if (req.catalog)
                    {
                        const auto def = req.catalog->find(def_id);
                        if (def.is_some() && !def.unwrap()->text.empty())
                            text = def.unwrap()->text;
                    }
                    out_ << card::display_name(req.catalog, def_id) << "：" << text
                         << "\n";
                }

                /** @brief 对手手牌候选项在候选列表中的遮挡占位文本。 */
                static constexpr const char *kHiddenHandPlaceholder = "（未知手牌）";

                /** @brief 查询对手手牌候选项牌文时的遮挡提示。 */
                static constexpr const char *kHiddenHandCardHint =
                    "（未知手牌，无法查看）";

                /**
                 * @brief 候选是否属于对手手牌、需向决策者遮挡内容。
                 * @param req   当前决策请求。
                 * @param index 候选的 0 基下标。
                 * @return 仅 PickCard 的手牌候选返回 true；缺少分区标签时防御性
                 *         返回 true（平行数组缺口不得导致漏遮）。
                 * @note 装备区/判定区为明置信息，其余决策类别的候选均为决策者
                 *       自己可见的牌，一律返回 false。此谓词是候选遮挡的唯一判据。
                 */
                static bool is_hidden_option(
                    const DecisionRequest &req, std::size_t index)
                {
                    if (req.kind != DecisionKind::PickCard)
                        return false;
                    if (index >= req.zone_labels.size())
                        return true;
                    return req.zone_labels[index] == card::Zone::Hand;
                }

                /**
                 * @brief 处理窗口内 `card <序号>`：打印该候选牌的效果文案。
                 * @tparam Candidates 候选容器类型。
                 * @tparam DefIdOf    从候选元素取卡牌定义 id 的一元函数。
                 * @param req        当前决策请求。
                 * @param token      序号 token。
                 * @param candidates 候选容器，序号与窗口打印的 1 基编号一致。
                 * @param def_id_of  候选 → 定义 id 投影。
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
                        out_ << kHiddenHandCardHint << "\n";
                        return;
                    }
                    print_card_text(
                        req, def_id_of(
                                 candidates[static_cast<std::size_t>(index - 1)]));
                }

                /** @brief 目标牌来源分区标签；非选牌决策返回空串。 */
                static const char *zone_tag(card::Zone zone)
                {
                    switch (zone)
                    {
                    case card::Zone::Hand:
                        return "[手]";
                    case card::Zone::Equip:
                        return "[装]";
                    case card::Zone::Judge:
                        return "[判]";
                    default:
                        return "";
                    }
                }

                /**
                 * @brief 打印 1 基编号的牌候选列表。
                 * @note 候选带来源分区标签（选目标牌）时在牌名前标注
                 *       `[手]/[装]/[判]`，其余决策无标签保持原样。选目标牌的
                 *       手牌候选只打印分区标签与遮挡占位，不显示牌名与实例号；
                 *       装备/判定区明置，照常显示。
                 */
                void print_options(
                    const DecisionRequest &req,
                    const std::vector<card::Card> &options)
                {
                    const bool has_zones = req.zone_labels.size() == options.size();
                    for (std::size_t i = 0; i < options.size(); ++i)
                    {
                        out_ << "  " << (i + 1) << ") ";
                        if (has_zones)
                            out_ << zone_tag(req.zone_labels[i]) << " ";
                        if (is_hidden_option(req, i))
                        {
                            out_ << kHiddenHandPlaceholder << "\n";
                            continue;
                        }
                        out_ << card::display_name(req.catalog, options[i].def_id)
                             << " " << options[i].instance_id << "\n";
                    }
                }

                /** @brief 打印重提示分隔行与具体原因，并附 `?` 出口，保留「输入无效」标识。 */
                void print_invalid(const std::string &reason)
                {
                    out_ << "\n输入无效：" << reason << "（" << kHelpHint << "）\n";
                }

                /** @brief 1 基序号的合法范围提示文本。 */
                static std::string index_hint(std::size_t count)
                {
                    return "序号需为 1-" + std::to_string(count) + " 之间的整数。";
                }

                /**
                 * @brief 响应窗口标题：有来源牌时给来源与伤害后果，否则回落
                 *        「需打出闪/杀」旧口径（兼容无上下文调用）。
                 * @param pair 是否两张手牌当杀窗口（丈八蛇矛）。
                 */
                std::string response_title(const DecisionRequest &req, bool pair)
                {
                    const bool jink =
                        req.response_kind == card::ResponseKind::Jink;
                    const std::string need =
                        pair ? "需打出两张手牌当杀"
                             : (jink ? "需打出闪" : "需打出杀");
                    if (req.response_source.empty())
                        return pair ? "响应（杀）：打出两张手牌当杀"
                                    : "响应（" + need + "）";

                    std::string title =
                        "响应（来源: " + req.response_user + " 的 " +
                        card::display_name(req.catalog, req.response_source) +
                        "，" + need;
                    if (req.response_damage > 0)
                        title += "；不出将受到 " +
                                 std::to_string(req.response_damage) + " 点伤害";
                    title += "）";
                    return title;
                }

                /** @brief 亮牌窗口标题：按来源结算分别渲染五谷丰登/麒麟弓/火攻。 */
                static std::string reveal_title(const DecisionRequest &req)
                {
                    switch (req.reveal_source)
                    {
                    case RevealSource::Qilin:
                        return "麒麟弓：选择目标坐骑";
                    case RevealSource::FireAttackReveal:
                        return "火攻：展示一张手牌";
                    case RevealSource::FireAttackDiscard:
                        return "火攻：弃一张同花色手牌（可放弃）";
                    case RevealSource::Wugu:
                        break;
                    }
                    return "五谷丰登亮牌（每名角色依次选一张）";
                }

                /** @brief 装备能力一句话效果（只读展示，不打印卡牌 text）。 */
                static const char *ability_hint(card::Ability ability)
                {
                    switch (ability)
                    {
                    case card::Ability::NoShaLimit:
                        return "出杀不受次数限制";
                    case card::Ability::IgnoreArmor:
                        return "无视目标的防具";
                    case card::Ability::Cixiong:
                        return "杀唯一异性目标时可令其弃一张手牌或令你摸一张";
                    case card::Ability::ExtraShaAfterJink:
                        return "杀被闪后可再出一张杀";
                    case card::Ability::TwoCardsAsSha:
                        return "两张手牌可当一张杀";
                    case card::Ability::DiscardTwoForceDamage:
                        return "弃两张牌令此杀依然造成伤害";
                    case card::Ability::MultiTargetSha:
                        return "杀为最后一张手牌时可额外指定目标";
                    case card::Ability::DiscardHorseOnDamage:
                        return "造成伤害后可弃置目标一匹坐骑";
                    case card::Ability::DamageAsDiscard:
                        return "可弃置目标两张牌以防止此伤害";
                    case card::Ability::JudgementJink:
                        return "需出闪时可判定，红色结果视为闪";
                    case card::Ability::BlackShaImmune:
                        return "黑色杀对你无效";
                    case card::Ability::VineArmor:
                        return "普通杀与南蛮/万箭对你无效，火焰伤害 +1";
                    case card::Ability::GudingBlade:
                        return "杀的目标没有手牌时此伤害 +1";
                    case card::Ability::SilverLion:
                        return "单次受到的伤害至多 1 点；失去此装备回复 1 点体力";
                    case card::Ability::FireShaConvert:
                        return "普通杀可当具火焰伤害的杀使用（可放弃）";
                    }
                    return "装备能力";
                }

                /**
                 * @brief 无懈窗口提示标题：锦囊使用者（判定窗口使用者不可考 →
                 *        延时判定标注）与目标集合；有锦囊名时一并渲染。
                 */
                static std::string counter_title(const DecisionRequest &req)
                {
                    std::string title = "无懈可击窗口";
                    if (req.counter_user.empty())
                    {
                        title += "（延时锦囊判定";
                        if (!req.counter_trick.empty())
                            title += "：" + card::display_name(
                                                 req.catalog, req.counter_trick);
                        title += "）";
                    }
                    else
                    {
                        title += "（使用者: " + req.counter_user;
                        if (!req.counter_trick.empty())
                            title += " 的 " + card::display_name(
                                                 req.catalog, req.counter_trick);
                        title += "）";
                    }
                    for (std::size_t i = 0; i < req.counter_targets.size(); ++i)
                    {
                        if (i > 0)
                            title += ",";
                        else
                            title += "目标: ";
                        title += req.counter_targets[i];
                    }
                    return title;
                }

                static const char *reason_text(DiscardReason reason)
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

                /** @brief 从装备区反查携带该能力的装备名；查不到回落「装备能力」。 */
                static std::string ability_name(const DecisionRequest &req)
                {
                    if (req.catalog)
                    {
                        for (const auto &c : req.view.equip)
                        {
                            const auto def = req.catalog->find(c.def_id);
                            if (def.is_none())
                                continue;
                            const auto &d = *def.unwrap();
                            for (const auto ability : d.abilities)
                                if (ability == req.ability)
                                    return card::display_name(d);
                        }
                    }
                    return "装备能力";
                }

                /**
                 * @brief 借刀杀人候选的受害者是否为决策者本人。
                 * @param req    当前出牌决策请求。
                 * @param act    待判定的合法动作。
                 * @param holder 出参：命中时写入持武器者 id（targets[0]）。
                 * @return 是则 true；非借刀/非双目标/受害者非本人则 false。
                 * @note 只读，不改变候选与输入；用于出牌窗口的后果提示。
                 */
                static bool is_self_target_borrowed_sword(
                    const DecisionRequest &req, const LegalAction &act,
                    std::string &holder)
                {
                    if (!req.catalog || act.targets.size() != 2 ||
                        act.targets[1] != req.actor)
                        return false;

                    const auto def = req.catalog->find(act.card.def_id);
                    if (def.is_none() || def.unwrap()->effect.is_none())
                        return false;
                    if (def.unwrap()->effect.unwrap().kind !=
                        card::CardEffectKind::BorrowedSword)
                        return false;

                    holder = act.targets[0];
                    return true;
                }

                DecisionChoice decide_play(const DecisionRequest &req)
                {
                    DecisionChoice out;
                    if (req.legal.empty())
                        return out;

                    for (;;)
                    {
                        out_ << "[" << req.actor << "] 出牌阶段：\n";
                        print_view(req);
                        for (std::size_t i = 0; i < req.legal.size(); ++i)
                        {
                            const auto &act = req.legal[i];
                            out_ << "  " << (i + 1) << ") "
                                 << card::display_name(req.catalog, act.card.def_id)
                                 << " " << act.card.instance_id;
                            if (!act.second_instance_id.empty())
                                out_ << " + " << act.second_instance_id;
                            if (!act.targets.empty())
                            {
                                out_ << " -> ";
                                for (std::size_t j = 0; j < act.targets.size(); ++j)
                                {
                                    if (j > 0)
                                        out_ << ",";
                                    out_ << act.targets[j];
                                }
                            }
                            if (act.recast)
                                out_ << "（重铸：弃置并摸一张）";
                            std::string holder;
                            if (is_self_target_borrowed_sword(req, act, holder))
                                out_ << "（警告：" << holder
                                     << " 将对你出杀，可能致你受伤或阵亡）";
                            out_ << "\n";
                        }
                        out_ << "输入 play <序号> 或 pass（" << kCardHint
                             << "）：" << std::flush;

                        const auto input = read_tokens();
                        if (input.is_none())
                            return out;

                        const auto &tokens = input.unwrap();
                        if (tokens.empty())
                            continue;
                        if (tokens.size() == 1 && tokens[0] == "pass")
                            return out;
                        if (tokens.size() == 2 && tokens[0] == "card")
                        {
                            show_card_or_hint(
                                req, tokens[1], req.legal,
                                [](const LegalAction &a) -> const std::string &
                                { return a.card.def_id; });
                            continue;
                        }
                        if (is_help(tokens))
                        {
                            print_help(
                                "输入 play <序号> 出牌（可连续出牌），"
                                "pass 结束出牌阶段。");
                            continue;
                        }

                        int index = 0;
                        if (tokens.size() == 2 && tokens[0] == "play")
                        {
                            if (parse_index(tokens[1], req.legal.size(), index))
                            {
                                const auto &act =
                                    req.legal[static_cast<std::size_t>(index - 1)];
                                out.instance_id = Option<std::string>::Some(
                                    act.card.instance_id);
                                out.targets = act.targets;
                                out.second_instance_id = act.second_instance_id;
                                out.recast = act.recast;
                                return out;
                            }
                            print_invalid(index_hint(req.legal.size()));
                            continue;
                        }
                        print_invalid("请输入 play <序号> 或 pass。");
                    }
                }

                /** @brief 单张手牌选择（响应/救桃/无懈）：play <n> 或 pass。 */
                DecisionChoice decide_choose_id(
                    const DecisionRequest &req, const std::string &title)
                {
                    DecisionChoice out;
                    if (req.options.empty())
                        return out;

                    for (;;)
                    {
                        out_ << "[" << req.actor << "] " << title << "：\n";
                        print_view(req);
                        print_options(req, req.options);
                        out_ << "输入 play <序号> 或 pass（" << kCardHint
                             << "）：" << std::flush;

                        const auto input = read_tokens();
                        if (input.is_none())
                            return out;

                        const auto &tokens = input.unwrap();
                        if (tokens.empty())
                            continue;
                        if (tokens.size() == 1 && tokens[0] == "pass")
                            return out;
                        if (tokens.size() == 2 && tokens[0] == "card")
                        {
                            show_card_or_hint(
                                req, tokens[1], req.options,
                                [](const card::Card &c) -> const std::string &
                                { return c.def_id; });
                            continue;
                        }
                        if (is_help(tokens))
                        {
                            print_help("输入 play <序号> 打出该牌，pass 放弃。");
                            continue;
                        }

                        int index = 0;
                        if (tokens.size() == 2 && tokens[0] == "play")
                        {
                            if (parse_index(tokens[1], req.options.size(), index))
                            {
                                out.instance_id = Option<std::string>::Some(
                                    req.options[static_cast<std::size_t>(index - 1)]
                                        .instance_id);
                                return out;
                            }
                            print_invalid(index_hint(req.options.size()));
                            continue;
                        }
                        print_invalid("请输入 play <序号> 或 pass。");
                    }
                }

                /**
                 * @brief 杀响应窗口、两张手牌当杀（丈八蛇矛）：手牌编号渲染，
                 *        读 `play <序号> + <序号>`；本模式下无真响应牌候选。
                 * @note 读取失败（EOF）按放弃处理并立即返回，绝不重提示。
                 */
                DecisionChoice decide_response_pair(const DecisionRequest &req)
                {
                    DecisionChoice out;
                    const auto &hand = req.view.hand;
                    if (hand.size() < 2)
                        return out;

                    for (;;)
                    {
                        out_ << "[" << req.actor << "] "
                             << response_title(req, true) << "：\n";
                        print_view(req);
                        for (std::size_t i = 0; i < hand.size(); ++i)
                            out_ << "  " << (i + 1) << ") "
                                 << card::display_name(req.catalog, hand[i].def_id)
                                 << " " << hand[i].instance_id << "\n";
                        out_ << "输入 play <序号> + <序号> 或 pass（"
                             << kCardHint << "）：" << std::flush;

                        const auto input = read_tokens();
                        if (input.is_none())
                            return out;

                        const auto &tokens = input.unwrap();
                        if (tokens.empty())
                            continue;
                        if (tokens.size() == 1 && tokens[0] == "pass")
                            return out;
                        if (tokens.size() == 2 && tokens[0] == "card")
                        {
                            show_card_or_hint(
                                req, tokens[1], hand,
                                [](const card::Card &c) -> const std::string &
                                { return c.def_id; });
                            continue;
                        }
                        if (is_help(tokens))
                        {
                            print_help(
                                "输入 play <序号> + <序号> 打出两张手牌当杀，"
                                "pass 放弃。");
                            continue;
                        }

                        if (tokens.size() == 4 && tokens[0] == "play" &&
                            tokens[2] == "+")
                        {
                            int first = 0, second = 0;
                            if (!parse_index(tokens[1], hand.size(), first) ||
                                !parse_index(tokens[3], hand.size(), second))
                            {
                                print_invalid(index_hint(hand.size()));
                                continue;
                            }
                            if (first == second)
                            {
                                print_invalid("两张牌的序号不能相同。");
                                continue;
                            }
                            out.instance_id = Option<std::string>::Some(
                                hand[static_cast<std::size_t>(first - 1)].instance_id);
                            out.second_instance_id =
                                hand[static_cast<std::size_t>(second - 1)].instance_id;
                            return out;
                        }
                        print_invalid("请输入 play <序号> + <序号> 或 pass。");
                    }
                }

                /**
                 * @brief 从牌池选一张：pick <序号>（allow_pass 时或 pass）。
                 * @param title 窗口标题（五谷/麒麟/目标区域，由调用点按来源定）。
                 * @param allow_pass 拆/顺允许放弃（None 由引擎按非法选择处理）；
                 *        五谷为强制选择，不提供 pass，输入 pass 视为非法重提示。
                 * @note EOF 在两种模式下都返回空选择，由引擎给出诊断。
                 */
                DecisionChoice decide_pick(
                    const DecisionRequest &req, const std::string &title,
                    bool allow_pass)
                {
                    DecisionChoice out;
                    if (req.options.empty())
                        return out;

                    for (;;)
                    {
                        out_ << "[" << req.actor << "] " << title;
                        if (!req.target.empty())
                            out_ << "（目标: " << req.target << "）";
                        out_ << "：\n";
                        print_view(req);
                        print_options(req, req.options);
                        if (allow_pass)
                            out_ << "输入 pick <序号> 或 pass（" << kCardHint
                                 << "）：" << std::flush;
                        else
                            out_ << "输入 pick <序号>（" << kCardHint
                                 << "）：" << std::flush;

                        const auto input = read_tokens();
                        if (input.is_none())
                            return out;

                        const auto &tokens = input.unwrap();
                        if (tokens.empty())
                            continue;
                        if (tokens.size() == 1 && tokens[0] == "pass")
                        {
                            if (allow_pass)
                                return out;
                            print_invalid("请输入 pick <序号>。");
                            continue;
                        }
                        if (tokens.size() == 2 && tokens[0] == "card")
                        {
                            show_card_or_hint(
                                req, tokens[1], req.options,
                                [](const card::Card &c) -> const std::string &
                                { return c.def_id; });
                            continue;
                        }
                        if (is_help(tokens))
                        {
                            print_help(
                                allow_pass
                                    ? "输入 pick <序号> 选择，pass 放弃。"
                                    : "输入 pick <序号> 选择（必须选一张）。");
                            continue;
                        }

                        int index = 0;
                        if (tokens.size() == 2 && tokens[0] == "pick")
                        {
                            if (parse_index(tokens[1], req.options.size(), index))
                            {
                                out.option_index = Option<std::size_t>::Some(
                                    static_cast<std::size_t>(index - 1));
                                return out;
                            }
                            print_invalid(index_hint(req.options.size()));
                            continue;
                        }
                        print_invalid(
                            allow_pass ? "请输入 pick <序号> 或 pass。"
                                       : "请输入 pick <序号>。");
                    }
                }

                DecisionChoice decide_discard(const DecisionRequest &req)
                {
                    DecisionChoice out;
                    if (req.count <= 0)
                        return out;

                    // 雌雄双股剑二选一：放弃弃牌即令使用者摸一张
                    const bool can_pass =
                        req.discard_reason == DiscardReason::CixiongChoice;

                    for (;;)
                    {
                        out_ << "[" << req.actor << "] 弃牌（"
                             << reason_text(req.discard_reason) << "，需弃 "
                             << req.count << " 张）：\n";
                        print_view(req);
                        print_options(req, req.options);
                        if (can_pass)
                            out_ << "输入 discard <序号> ... 或 pass（放弃弃牌；"
                                 << kCardHint << "）：" << std::flush;
                        else
                            out_ << "输入 discard <序号> ...（" << kCardHint
                                 << "）：" << std::flush;

                        const auto input = read_tokens();
                        if (input.is_none())
                            return out;  // EOF：空选择，交由引擎报数量不足

                        const auto &tokens = input.unwrap();
                        if (tokens.empty())
                            continue;
                        if (can_pass && tokens.size() == 1 && tokens[0] == "pass")
                            return out;
                        if (tokens.size() == 2 && tokens[0] == "card")
                        {
                            show_card_or_hint(
                                req, tokens[1], req.options,
                                [](const card::Card &c) -> const std::string &
                                { return c.def_id; });
                            continue;
                        }
                        if (is_help(tokens))
                        {
                            print_help(
                                "输入 discard <序号> ... 弃置 " +
                                std::to_string(req.count) + " 张牌" +
                                (can_pass ? "；pass 放弃弃牌。" : "。"));
                            continue;
                        }
                        if (!can_pass && tokens.size() == 1 &&
                            tokens[0] == "pass")
                        {
                            print_invalid(
                                "弃牌阶段不能 pass：需弃 " +
                                std::to_string(req.count) +
                                " 张，请输入 discard <序号> ...。");
                            continue;
                        }
                        if (tokens[0] != "discard")
                        {
                            print_invalid(
                                "请输入 discard <序号> ...（需弃 " +
                                std::to_string(req.count) + " 张）。");
                            continue;
                        }
                        if (tokens.size() !=
                            static_cast<std::size_t>(req.count) + 1)
                        {
                            print_invalid(
                                "需弃 " + std::to_string(req.count) +
                                " 张牌，请给出 " + std::to_string(req.count) +
                                " 个序号。");
                            continue;
                        }

                        std::vector<std::size_t> chosen;
                        bool valid = true;
                        for (std::size_t i = 1; i < tokens.size() && valid; ++i)
                        {
                            int index = 0;
                            if (!parse_index(tokens[i], req.options.size(), index))
                            {
                                print_invalid(index_hint(req.options.size()));
                                valid = false;
                                break;
                            }
                            const auto pos = static_cast<std::size_t>(index - 1);
                            if (std::find(chosen.begin(), chosen.end(), pos) !=
                                chosen.end())
                            {
                                print_invalid("序号不能重复。");
                                valid = false;
                                break;
                            }
                            chosen.push_back(pos);
                        }
                        if (!valid)
                            continue;

                        for (const auto pos : chosen)
                            out.discards.push_back(req.options[pos].instance_id);
                        return out;
                    }
                }

                DecisionChoice decide_trigger(const DecisionRequest &req)
                {
                    DecisionChoice out;
                    for (;;)
                    {
                        print_view(req);
                        out_ << "[" << req.actor << "] 发动 " << ability_name(req)
                             << "（" << ability_hint(req.ability) << "）？(y/n)（"
                             << kHelpHint << "）：" << std::flush;

                        const auto input = read_tokens();
                        if (input.is_none())
                            return out;  // EOF：不发动

                        const auto &tokens = input.unwrap();
                        if (tokens.empty())
                            continue;
                        if (is_help(tokens))
                        {
                            print_help("输入 y 发动，n 不发动。");
                            continue;
                        }
                        if (tokens.size() == 1 &&
                            (tokens[0] == "y" || tokens[0] == "yes"))
                        {
                            out.accepted = true;
                            return out;
                        }
                        if (tokens.size() == 1 &&
                            (tokens[0] == "n" || tokens[0] == "no"))
                            return out;
                        print_invalid("请输入 y 或 n。");
                    }
                }
            };

            /**
             * @class HumanAI
             * @brief 引擎可用的真人决策源：HumanDecider 经适配器接入。
             * @note 输入/输出流由调用方持有，其生命周期须覆盖本对象。
             */
            class HumanAI : private HumanDecider, public RequestDecisionSource
            {
            public:
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
             */
            class RoutedAI : public DecisionSource
            {
            public:
                RoutedAI(
                    const std::vector<std::string> &humans, std::istream &in,
                    std::ostream &out) :
                    RoutedAI(humans, in, out, std::make_unique<SimpleAI>())
                {
                }

                RoutedAI(
                    const std::vector<std::string> &humans, std::istream &in,
                    std::ostream &out, std::unique_ptr<DecisionSource> fallback) :
                    fallback_(std::move(fallback))
                {
                    for (const auto &id : humans)
                        humans_.emplace(id, std::make_unique<HumanAI>(in, out));
                }

                Option<PlayAction> play_response(
                    const ReadOnlyContext &ctx, const std::string &entity,
                    card::ResponseKind kind, const ResponsePrompt &prompt) override
                {
                    return route(entity).play_response(ctx, entity, kind, prompt);
                }

                Option<TargetPick> pick_card_from_target(
                    const ReadOnlyContext &ctx, const std::string &source,
                    const std::string &target, PickCardScope scope) override
                {
                    return route(source).pick_card_from_target(
                        ctx, source, target, scope);
                }

                Option<PlayAction> choose_play(
                    const ReadOnlyContext &ctx, const TurnContext &turn) override
                {
                    return route(turn.player).choose_play(ctx, turn);
                }

                std::vector<std::string> choose_discards(
                    const ReadOnlyContext &ctx, const std::string &player, int count,
                    DiscardReason reason) override
                {
                    return route(player).choose_discards(ctx, player, count, reason);
                }

                Option<card::Card> pick_from_revealed(
                    const ReadOnlyContext &ctx, const std::string &player,
                    const std::vector<card::Card> &options,
                    RevealSource source) override
                {
                    return route(player).pick_from_revealed(
                        ctx, player, options, source);
                }

                Option<std::string> play_peach(
                    const ReadOnlyContext &ctx, const std::string &saver,
                    const std::string &dying) override
                {
                    return route(saver).play_peach(ctx, saver, dying);
                }

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

                bool trigger_effect(
                    const ReadOnlyContext &ctx, const std::string &player,
                    card::Ability ability) override
                {
                    return route(player).trigger_effect(ctx, player, ability);
                }

            private:
                std::unique_ptr<DecisionSource> fallback_;
                std::map<std::string, std::unique_ptr<HumanAI>> humans_;

                DecisionSource &route(const std::string &actor)
                {
                    const auto it = humans_.find(actor);
                    if (it != humans_.end())
                        return static_cast<DecisionSource &>(*it->second);
                    return *fallback_;
                }
            };
        }
    }
}

#endif  // INCLUDE_TKW_GAME_HUMAN_HPP
