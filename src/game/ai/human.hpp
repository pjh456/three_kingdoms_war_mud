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
                        return decide_choose_id(request, "响应");
                    case DecisionKind::Peach:
                        return decide_choose_id(
                            request, "濒死救场（濒死者: " + request.dying + "）");
                    case DecisionKind::Counter:
                        return decide_choose_id(request, "无懈可击窗口");
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

                /** @brief 打印 1 基编号的牌候选列表。 */
                void print_options(
                    const DecisionRequest &req,
                    const std::vector<card::Card> &options)
                {
                    for (std::size_t i = 0; i < options.size(); ++i)
                        out_ << "  " << (i + 1) << ") "
                             << card_name(req, options[i].def_id) << " "
                             << options[i].instance_id << "\n";
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
                        for (std::size_t i = 0; i < req.legal.size(); ++i)
                        {
                            const auto &act = req.legal[i];
                            out_ << "  " << (i + 1) << ") "
                                 << card_name(req, act.card.def_id) << " "
                                 << act.card.instance_id;
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
                        if (tokens.size() == 1 && tokens[0] == "pass")
                            return out;

                        int index = 0;
                        if (tokens.size() == 2 && tokens[0] == "play" &&
                            parse_index(tokens[1], req.legal.size(), index))
                        {
                            const auto &act =
                                req.legal[static_cast<std::size_t>(index - 1)];
                            out.instance_id =
                                Option<std::string>::Some(act.card.instance_id);
                            out.targets = act.targets;
                            return out;
                        }
                        out_ << "输入无效，请重试。\n";
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
                        print_options(req, req.options);
                        out_ << "输入 play <序号> 或 pass：" << std::flush;

                        std::string line;
                        if (!read_line(line))
                            return out;

                        const auto tokens = tokenize(line);
                        if (tokens.size() == 1 && tokens[0] == "pass")
                            return out;

                        int index = 0;
                        if (tokens.size() == 2 && tokens[0] == "play" &&
                            parse_index(tokens[1], req.options.size(), index))
                        {
                            out.instance_id = Option<std::string>::Some(
                                req.options[static_cast<std::size_t>(index - 1)]
                                    .instance_id);
                            return out;
                        }
                        out_ << "输入无效，请重试。\n";
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
                        print_options(req, req.options);
                        out_ << "输入 pick <序号> 或 pass：" << std::flush;

                        std::string line;
                        if (!read_line(line))
                            return out;

                        const auto tokens = tokenize(line);
                        if (tokens.size() == 1 && tokens[0] == "pass")
                            return out;

                        int index = 0;
                        if (tokens.size() == 2 && tokens[0] == "pick" &&
                            parse_index(tokens[1], req.options.size(), index))
                        {
                            out.card = Option<card::Card>::Some(
                                req.options[static_cast<std::size_t>(index - 1)]);
                            return out;
                        }
                        out_ << "输入无效，请重试。\n";
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
                        print_options(req, req.options);
                        out_ << "输入 discard <下标> ...：" << std::flush;

                        std::string line;
                        if (!read_line(line))
                            return out;  // EOF：空选择，交由引擎报数量不足

                        const auto tokens = tokenize(line);
                        if (tokens.size() ==
                                static_cast<std::size_t>(req.count) + 1 &&
                            tokens[0] == "discard")
                        {
                            std::vector<std::size_t> chosen;
                            bool valid = true;
                            for (std::size_t i = 1; i < tokens.size() && valid; ++i)
                            {
                                int index = 0;
                                if (!parse_index(
                                        tokens[i], req.options.size(), index))
                                {
                                    valid = false;
                                    break;
                                }
                                const auto pos = static_cast<std::size_t>(index - 1);
                                if (std::find(chosen.begin(), chosen.end(), pos) !=
                                    chosen.end())
                                {
                                    valid = false;
                                    break;
                                }
                                chosen.push_back(pos);
                            }
                            if (valid)
                            {
                                for (const auto pos : chosen)
                                    out.discards.push_back(
                                        req.options[pos].instance_id);
                                return out;
                            }
                        }
                        out_ << "输入无效，请重试。\n";
                    }
                }

                DecisionChoice decide_trigger(const DecisionRequest &req)
                {
                    DecisionChoice out;
                    for (;;)
                    {
                        out_ << "[" << req.actor << "] 发动 " << ability_name(req)
                             << "？(y/n)：" << std::flush;

                        std::string line;
                        if (!read_line(line))
                            return out;  // EOF：不发动

                        const auto tokens = tokenize(line);
                        if (tokens.size() == 1 &&
                            (tokens[0] == "y" || tokens[0] == "yes"))
                        {
                            out.accepted = true;
                            return out;
                        }
                        if (tokens.size() == 1 &&
                            (tokens[0] == "n" || tokens[0] == "no"))
                            return out;
                        out_ << "输入无效，请重试。\n";
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
             * @brief 按决策者 id 路由：命中真人座位走交互输入，其余回落贪心 AI。
             * @note 每个回调携带的 actor 即该决策的发起者（出牌为回合角色，响应/
             *       救桃/无懈/弃牌/选牌为当事人）；路由只按 id 查表，不改接缝。
             */
            class RoutedAI : public DecisionSource
            {
            public:
                RoutedAI(
                    const std::vector<std::string> &humans, std::istream &in,
                    std::ostream &out)
                {
                    for (const auto &id : humans)
                        humans_.emplace(id, std::make_unique<HumanAI>(in, out));
                }

                Option<std::string> play_response(
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
                    const GameContext &ctx, const std::string &player) override
                {
                    return route(player).play_counter(ctx, player);
                }

                bool trigger_effect(
                    const GameContext &ctx, const std::string &player,
                    card::Ability ability) override
                {
                    return route(player).trigger_effect(ctx, player, ability);
                }

            private:
                SimpleAI fallback_;
                std::map<std::string, std::unique_ptr<HumanAI>> humans_;

                DecisionSource &route(const std::string &actor)
                {
                    const auto it = humans_.find(actor);
                    if (it != humans_.end())
                        return static_cast<DecisionSource &>(*it->second);
                    return static_cast<DecisionSource &>(fallback_);
                }
            };
        }
    }
}

#endif  // INCLUDE_TKW_GAME_AI_HUMAN_HPP
