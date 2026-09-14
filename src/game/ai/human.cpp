/**
 * @file   human.cpp
 * @brief  真人交互决策与按座位路由的定义。
 * @ingroup tkw_game_ai
 */

#include "game/ai/human.hpp"

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

namespace tkw
{
    namespace game
    {
        namespace ai
        {
            DecisionChoice HumanDecider::decide(const DecisionRequest &request)
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
                        m_out << "无可用响应牌。\n";
                        return DecisionChoice{};
                    }
                    return decide_choose_id(
                        request, response_title(request, false));
                case DecisionKind::Peach:
                    if (request.options.empty())
                    {
                        m_out << "无可用救场牌。\n";
                        return DecisionChoice{};
                    }
                    return decide_choose_id(
                        request,
                        "濒死救场（濒死者: " + request.dying + "）");
                case DecisionKind::Counter:
                    if (request.options.empty())
                    {
                        m_out << "无可用无懈可击。\n";
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

            bool HumanDecider::read_line(std::string &line)
            {
                if (!std::getline(m_in, line))
                    return false;
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                return true;
            }

            std::vector<std::string> HumanDecider::tokenize(const std::string &line)
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

            Option<std::vector<std::string>> HumanDecider::read_tokens()
            {
                std::string line;
                if (!read_line(line))
                {
                    m_out << "\n输入已结束，按放弃处理。\n";
                    return Option<std::vector<std::string>>::None();
                }

                auto tokens = tokenize(line);
                if (tokens.empty())
                    m_out << "\n";
                return Option<std::vector<std::string>>::Some(std::move(tokens));
            }

            bool HumanDecider::parse_index(
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

            std::string HumanDecider::zone_names(
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

            void HumanDecider::print_view(const DecisionRequest &req)
            {
                const auto &v = req.view;
                m_out << "[" << v.self << "] 体力 " << v.self_hp << "/"
                     << v.self_max_hp << "  手牌 " << zone_names(req, v.hand)
                     << "  装备 " << zone_names(req, v.equip) << "  判定 "
                     << zone_names(req, v.judge) << "\n";
                for (const auto &e : v.others)
                    m_out << e.id << " 体力 " << e.hp << "/" << e.max_hp
                         << "  手牌 " << e.hand_size << "  装备 "
                         << zone_names(req, e.equip) << "  距离 " << e.distance
                         << "\n";
            }

            void HumanDecider::print_card_text(
                const DecisionRequest &req, const std::string &def_id)
            {
                std::string text = "（无说明）";
                if (req.catalog)
                {
                    const auto def = req.catalog->find(def_id);
                    if (def.is_some() && !def.unwrap()->text.empty())
                        text = def.unwrap()->text;
                }
                m_out << card::display_name(req.catalog, def_id) << "：" << text
                     << "\n";
            }

            bool HumanDecider::is_hidden_option(
                const DecisionRequest &req, std::size_t index)
            {
                if (req.kind != DecisionKind::PickCard)
                    return false;
                if (index >= req.zone_labels.size())
                    return true;
                return req.zone_labels[index] == card::Zone::Hand;
            }

            const char *HumanDecider::zone_tag(card::Zone zone)
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

            void HumanDecider::print_options(
                const DecisionRequest &req,
                const std::vector<card::Card> &options)
            {
                const bool has_zones = req.zone_labels.size() == options.size();
                for (std::size_t i = 0; i < options.size(); ++i)
                {
                    m_out << "  " << (i + 1) << ") ";
                    if (has_zones)
                        m_out << zone_tag(req.zone_labels[i]) << " ";
                    if (is_hidden_option(req, i))
                    {
                        m_out << kHiddenHandPlaceholder << "\n";
                        continue;
                    }
                    m_out << card::display_name(req.catalog, options[i].def_id)
                         << " " << options[i].instance_id << "\n";
                }
            }

            std::string HumanDecider::response_title(
                const DecisionRequest &req, bool pair)
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

            std::string HumanDecider::reveal_title(const DecisionRequest &req)
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

            const char *HumanDecider::ability_hint(card::Ability ability)
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

            std::string HumanDecider::counter_title(const DecisionRequest &req)
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

            std::string HumanDecider::ability_name(const DecisionRequest &req)
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

            const char *HumanDecider::hero_skill_hint(hero::HeroSkill skill)
            {
                switch (skill)
                {
                case hero::HeroSkill::FanKui:
                    return "受到伤害后，可获得伤害来源一张牌";
                case hero::HeroSkill::PaoXiao:
                case hero::HeroSkill::WuSheng:
                case hero::HeroSkill::YingZi:
                case hero::HeroSkill::MaShu:
                case hero::HeroSkill::QiCai:
                case hero::HeroSkill::LongDan:
                case hero::HeroSkill::QingGuo:
                    return "武将技能";
                }
                return "武将技能";
            }

            std::string HumanDecider::trigger_name(const DecisionRequest &req)
            {
                if (req.hero_trigger)
                    return hero::display_skill_name(req.hero_skill);
                return ability_name(req);
            }

            std::string HumanDecider::trigger_hint(const DecisionRequest &req)
            {
                if (req.hero_trigger)
                    return hero_skill_hint(req.hero_skill);
                return ability_hint(req.ability);
            }

            bool HumanDecider::is_self_target_borrowed_sword(
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

            DecisionChoice HumanDecider::decide_play(const DecisionRequest &req)
            {
                DecisionChoice out;
                if (req.legal.empty())
                    return out;

                for (;;)
                {
                    m_out << "[" << req.actor << "] 出牌阶段：\n";
                    print_view(req);
                    for (std::size_t i = 0; i < req.legal.size(); ++i)
                    {
                        const auto &act = req.legal[i];
                        m_out << "  " << (i + 1) << ") "
                             << card::display_name(req.catalog, act.card.def_id)
                             << " " << act.card.instance_id;
                        if (act.converted_sha)
                            m_out << "（当杀）";
                        if (!act.second_instance_id.empty())
                            m_out << " + " << act.second_instance_id;
                        if (!act.targets.empty())
                        {
                            m_out << " -> ";
                            for (std::size_t j = 0; j < act.targets.size(); ++j)
                            {
                                if (j > 0)
                                    m_out << ",";
                                m_out << act.targets[j];
                            }
                        }
                        if (act.recast)
                            m_out << "（重铸：弃置并摸一张）";
                        std::string holder;
                        if (is_self_target_borrowed_sword(req, act, holder))
                            m_out << "（警告：" << holder
                                 << " 将对你出杀，可能致你受伤或阵亡）";
                        m_out << "\n";
                    }
                    m_out << "输入 play <序号> 或 pass（" << kCardHint
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
                            out.converted_sha = act.converted_sha;
                            return out;
                        }
                        print_invalid(index_hint(req.legal.size()));
                        continue;
                    }
                    print_invalid("请输入 play <序号> 或 pass。");
                }
            }

            DecisionChoice HumanDecider::decide_choose_id(
                const DecisionRequest &req, const std::string &title)
            {
                DecisionChoice out;
                if (req.options.empty())
                    return out;

                for (;;)
                {
                    m_out << "[" << req.actor << "] " << title << "：\n";
                    print_view(req);
                    print_options(req, req.options);
                    m_out << "输入 play <序号> 或 pass（" << kCardHint
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

            DecisionChoice HumanDecider::decide_response_pair(
                const DecisionRequest &req)
            {
                DecisionChoice out;
                const auto &hand = req.view.hand;
                if (hand.size() < 2)
                    return out;

                for (;;)
                {
                    m_out << "[" << req.actor << "] "
                         << response_title(req, true) << "：\n";
                    print_view(req);
                    for (std::size_t i = 0; i < hand.size(); ++i)
                        m_out << "  " << (i + 1) << ") "
                             << card::display_name(req.catalog, hand[i].def_id)
                             << " " << hand[i].instance_id << "\n";
                    m_out << "输入 play <序号> + <序号> 或 pass（"
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

            DecisionChoice HumanDecider::decide_pick(
                const DecisionRequest &req, const std::string &title,
                bool allow_pass)
            {
                DecisionChoice out;
                if (req.options.empty())
                    return out;

                for (;;)
                {
                    m_out << "[" << req.actor << "] " << title;
                    if (!req.target.empty())
                        m_out << "（目标: " << req.target << "）";
                    m_out << "：\n";
                    print_view(req);
                    print_options(req, req.options);
                    if (allow_pass)
                        m_out << "输入 pick <序号> 或 pass（" << kCardHint
                             << "）：" << std::flush;
                    else
                        m_out << "输入 pick <序号>（" << kCardHint
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

            DecisionChoice HumanDecider::decide_discard(const DecisionRequest &req)
            {
                DecisionChoice out;
                if (req.count <= 0)
                    return out;

                // 雌雄双股剑二选一：放弃弃牌即令使用者摸一张
                const bool can_pass =
                    req.discard_reason == DiscardReason::CixiongChoice;

                for (;;)
                {
                    m_out << "[" << req.actor << "] 弃牌（"
                         << game::discard_reason_text(req.discard_reason)
                         << "，需弃 "
                         << req.count << " 张）：\n";
                    print_view(req);
                    print_options(req, req.options);
                    if (can_pass)
                        m_out << "输入 discard <序号> ... 或 pass（放弃弃牌；"
                             << kCardHint << "）：" << std::flush;
                    else
                        m_out << "输入 discard <序号> ...（" << kCardHint
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

            DecisionChoice HumanDecider::decide_trigger(const DecisionRequest &req)
            {
                DecisionChoice out;
                for (;;)
                {
                    print_view(req);
                    m_out << "[" << req.actor << "] 发动 " << trigger_name(req)
                         << "（" << trigger_hint(req) << "）？(y/n)（"
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
        }
    }
}
