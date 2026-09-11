/**
 * @file human.hpp
 * @brief 真人决策接入：从输入流读取玩家选择，供 CLI 交互对局使用。
 * @note 只读契约不变：HumanDecider 只消费输入流并返回选择，不触碰对局状态；
 *       读取输入是决策源自身的副作用，串行 REPL 下与命令行解析无双读。
 *       候选按 1 起编号；非法输入重提示；EOF 一律视为放弃并立即返回。
 */

#ifndef INCLUDE_TKW_GAME_AI_HUMAN_HPP
#define INCLUDE_TKW_GAME_AI_HUMAN_HPP

#include <algorithm>
#include <charconv>
#include <cctype>
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
                        return decide_choose_id(
                            request,
                            request.response_kind == card::ResponseKind::Jink
                                ? "响应（需打出闪）"
                                : "响应（需打出杀）");
                    case DecisionKind::Peach:
                        return decide_choose_id(
                            request, "濒死救场（濒死者: " + request.dying + "）");
                    case DecisionKind::Counter:
                        return decide_choose_id(request, counter_title(request));
                    case DecisionKind::Trigger:
                        return decide_trigger(request);
                    case DecisionKind::PickCard:
                        return decide_pick(request, "选择目标区域的牌");
                    case DecisionKind::PickRevealed:
                        return decide_pick(request, "从亮出的牌中选择");
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

                /** @brief 卡名优先取目录 name，缺失回落 def_id（代码不硬编码牌名）。 */
                static std::string card_name(
                    const DecisionRequest &req, const std::string &def_id)
                {
                    if (req.catalog)
                    {
                        const auto def = req.catalog->find(def_id);
                        if (def.is_some() && !def.unwrap()->name.empty())
                            return def.unwrap()->name;
                    }
                    return def_id;
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
                        out += card_name(req, zone[i].def_id);
                    }
                    return out;
                }

                /**
                 * @brief 打印决策者视角的局面摘要：己方体力/手牌数/装备/判定，
                 *        其余角色逐行给体力/手牌数/装备/距离。
                 * @note 严格渲染 req.view 的可见性边界：双方手牌都只出数量，
                 *       绝不展开牌面内容。
                 * @note 纯展示，不改变候选与输入语法；每次重提示都会重绘。
                 */
                void print_view(const DecisionRequest &req)
                {
                    const auto &v = req.view;
                    out_ << "[" << v.self << "] 体力 " << v.self_hp << "/"
                         << v.self_max_hp << "  手牌 " << v.hand.size()
                         << "  装备 " << zone_names(req, v.equip) << "  判定 "
                         << zone_names(req, v.judge) << "\n";
                    for (const auto &e : v.others)
                        out_ << e.id << " 体力 " << e.hp << "/" << e.max_hp
                             << "  手牌 " << e.hand_size << "  装备 "
                             << zone_names(req, e.equip) << "  距离 " << e.distance
                             << "\n";
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
                 *       `[手]/[装]/[判]`，其余决策无标签保持原样。
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
                        out_ << card_name(req, options[i].def_id) << " "
                             << options[i].instance_id << "\n";
                    }
                }

                /** @brief 打印重提示分隔行与具体原因，保留「输入无效」标识。 */
                void print_invalid(const std::string &reason)
                {
                    out_ << "\n输入无效：" << reason << "\n";
                }

                /** @brief 1 基序号的合法范围提示文本。 */
                static std::string index_hint(std::size_t count)
                {
                    return "序号需为 1-" + std::to_string(count) + " 之间的整数。";
                }

                /**
                 * @brief 无懈窗口提示标题：锦囊使用者（判定窗口使用者不可考 →
                 *        延时判定标注）与目标集合。
                 */
                static std::string counter_title(const DecisionRequest &req)
                {
                    std::string title = "无懈可击窗口";
                    if (req.counter_user.empty())
                        title += "（延时锦囊判定）";
                    else
                        title += "（使用者: " + req.counter_user + "）";
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
                                    return d.name.empty() ? d.id : d.name;
                        }
                    }
                    return "装备能力";
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
                                 << card_name(req, act.card.def_id) << " "
                                 << act.card.instance_id;
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
                            out_ << "\n";
                        }
                        out_ << "输入 play <序号> 或 pass：" << std::flush;

                        std::string line;
                        if (!read_line(line))
                            return out;

                        const auto tokens = tokenize(line);
                        if (tokens.empty())
                        {
                            out_ << "\n";
                            continue;
                        }
                        if (tokens.size() == 1 && tokens[0] == "pass")
                            return out;

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
                        out_ << "输入 play <序号> 或 pass：" << std::flush;

                        std::string line;
                        if (!read_line(line))
                            return out;

                        const auto tokens = tokenize(line);
                        if (tokens.empty())
                        {
                            out_ << "\n";
                            continue;
                        }
                        if (tokens.size() == 1 && tokens[0] == "pass")
                            return out;

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
                        out_ << "[" << req.actor << "] 响应（杀）：打出两张手牌当杀：\n";
                        print_view(req);
                        for (std::size_t i = 0; i < hand.size(); ++i)
                            out_ << "  " << (i + 1) << ") "
                                 << card_name(req, hand[i].def_id) << " "
                                 << hand[i].instance_id << "\n";
                        out_ << "输入 play <序号> + <序号> 或 pass：" << std::flush;

                        std::string line;
                        if (!read_line(line))
                            return out;

                        const auto tokens = tokenize(line);
                        if (tokens.empty())
                        {
                            out_ << "\n";
                            continue;
                        }
                        if (tokens.size() == 1 && tokens[0] == "pass")
                            return out;

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

                /** @brief 从牌池选一张（拆/顺/五谷）：pick <n> 或 pass。 */
                DecisionChoice decide_pick(
                    const DecisionRequest &req, const char *title)
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
                        out_ << "输入 pick <序号> 或 pass：" << std::flush;

                        std::string line;
                        if (!read_line(line))
                            return out;

                        const auto tokens = tokenize(line);
                        if (tokens.empty())
                        {
                            out_ << "\n";
                            continue;
                        }
                        if (tokens.size() == 1 && tokens[0] == "pass")
                            return out;

                        int index = 0;
                        if (tokens.size() == 2 && tokens[0] == "pick")
                        {
                            if (parse_index(tokens[1], req.options.size(), index))
                            {
                                out.card = Option<card::Card>::Some(
                                    req.options[static_cast<std::size_t>(index - 1)]);
                                return out;
                            }
                            print_invalid(index_hint(req.options.size()));
                            continue;
                        }
                        print_invalid("请输入 pick <序号> 或 pass。");
                    }
                }

                DecisionChoice decide_discard(const DecisionRequest &req)
                {
                    DecisionChoice out;
                    if (req.count <= 0)
                        return out;

                    for (;;)
                    {
                        out_ << "[" << req.actor << "] 弃牌（"
                             << reason_text(req.discard_reason) << "，需弃 "
                             << req.count << " 张）：\n";
                        print_view(req);
                        print_options(req, req.options);
                        out_ << "输入 discard <序号> ...：" << std::flush;

                        std::string line;
                        if (!read_line(line))
                            return out;  // EOF：空选择，交由引擎报数量不足

                        const auto tokens = tokenize(line);
                        if (tokens.empty())
                        {
                            out_ << "\n";
                            continue;
                        }
                        if (tokens[0] != "discard")
                        {
                            print_invalid("请输入 discard <序号> ...。");
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
                             << "？(y/n)：" << std::flush;

                        std::string line;
                        if (!read_line(line))
                            return out;  // EOF：不发动

                        const auto tokens = tokenize(line);
                        if (tokens.empty())
                        {
                            out_ << "\n";
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
             *       回落决策源构造注入（难度档同源注入点）；三参构造保持贪心档
             *       回落，供既有调用点沿用。
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
                    const GameContext &ctx, const std::string &entity,
                    card::ResponseKind kind) override
                {
                    return route(entity).play_response(ctx, entity, kind);
                }

                Option<card::Card> pick_card_from_target(
                    const GameContext &ctx, const std::string &source,
                    const std::string &target) override
                {
                    return route(source).pick_card_from_target(ctx, source, target);
                }

                Option<PlayAction> choose_play(
                    const GameContext &ctx, const TurnContext &turn) override
                {
                    return route(turn.player).choose_play(ctx, turn);
                }

                std::vector<std::string> choose_discards(
                    const GameContext &ctx, const std::string &player, int count,
                    DiscardReason reason) override
                {
                    return route(player).choose_discards(ctx, player, count, reason);
                }

                Option<card::Card> pick_from_revealed(
                    const GameContext &ctx, const std::string &player,
                    const std::vector<card::Card> &options) override
                {
                    return route(player).pick_from_revealed(ctx, player, options);
                }

                Option<std::string> play_peach(
                    const GameContext &ctx, const std::string &saver,
                    const std::string &dying) override
                {
                    return route(saver).play_peach(ctx, saver, dying);
                }

                Option<std::string> play_counter(
                    const GameContext &ctx, const std::string &player,
                    const std::string &trick_user,
                    const std::vector<std::string> &trick_targets) override
                {
                    return route(player).play_counter(
                        ctx, player, trick_user, trick_targets);
                }

                bool trigger_effect(
                    const GameContext &ctx, const std::string &player,
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

#endif  // INCLUDE_TKW_GAME_AI_HUMAN_HPP
