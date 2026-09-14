/**
 * @file   decision_source.cpp
 * @brief  TUI 真人决策接缝实现：把决策请求折成纯值面板，经阻塞握手交给 UI 线程。
 * @ingroup tkw_tui
 */
#include "tui/decision_source.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace tkw
{
    namespace tui
    {
        namespace detail
        {
            const char *zone_tag(tkw::card::Zone zone)
            {
                switch (zone)
                {
                case tkw::card::Zone::Hand:
                    return "[手]";
                case tkw::card::Zone::Equip:
                    return "[装]";
                case tkw::card::Zone::Judge:
                    return "[判]";
                default:
                    return "";
                }
            }

            const char *ability_hint(tkw::card::Ability ability)
            {
                switch (ability)
                {
                case tkw::card::Ability::NoShaLimit:
                    return "出杀不受次数限制";
                case tkw::card::Ability::IgnoreArmor:
                    return "无视目标的防具";
                case tkw::card::Ability::Cixiong:
                    return "杀唯一异性目标时可令其弃一张手牌或令你摸一张";
                case tkw::card::Ability::ExtraShaAfterJink:
                    return "杀被闪后可再出一张杀";
                case tkw::card::Ability::TwoCardsAsSha:
                    return "两张手牌可当一张杀";
                case tkw::card::Ability::DiscardTwoForceDamage:
                    return "弃两张牌令此杀依然造成伤害";
                case tkw::card::Ability::MultiTargetSha:
                    return "杀为最后一张手牌时可额外指定目标";
                case tkw::card::Ability::DiscardHorseOnDamage:
                    return "造成伤害后可弃置目标一匹坐骑";
                case tkw::card::Ability::DamageAsDiscard:
                    return "可弃置目标两张牌以防止此伤害";
                case tkw::card::Ability::JudgementJink:
                    return "需出闪时可判定，红色结果视为闪";
                case tkw::card::Ability::BlackShaImmune:
                    return "黑色杀对你无效";
                case tkw::card::Ability::VineArmor:
                    return "普通杀与南蛮/万箭对你无效，火焰伤害 +1";
                case tkw::card::Ability::GudingBlade:
                    return "杀的目标没有手牌时此伤害 +1";
                case tkw::card::Ability::SilverLion:
                    return "单次受到的伤害至多 1 点；失去此装备回复 1 点体力";
                case tkw::card::Ability::FireShaConvert:
                    return "普通杀可当具火焰伤害的杀使用（可放弃）";
                }
                return "装备能力";
            }

            std::string ability_name(
                const tkw::game::ai::DecisionRequest &req)
            {
                if (req.catalog)
                {
                    for (const auto &c : req.view.equip)
                    {
                        const auto def = req.catalog->find(c.def_id);
                        if (def.is_none())
                            continue;
                        for (const auto ability : def.unwrap()->abilities)
                            if (ability == req.ability)
                                return tkw::card::display_name(*def.unwrap());
                    }
                }
                return "装备能力";
            }

            std::string hero_trigger_title(
                const tkw::game::ai::DecisionRequest &req)
            {
                std::string body = "武将技能";
                switch (req.hero_skill)
                {
                case tkw::hero::HeroSkill::FanKui:
                    if (!req.trigger_cause.empty())
                        body = "受到 " + req.trigger_cause +
                               " 的伤害后，可获得其一张牌";
                    else
                        body = "受到伤害后，可获得伤害来源一张牌";
                    break;
                case tkw::hero::HeroSkill::PaoXiao:
                case tkw::hero::HeroSkill::WuSheng:
                case tkw::hero::HeroSkill::YingZi:
                case tkw::hero::HeroSkill::MaShu:
                case tkw::hero::HeroSkill::QiCai:
                case tkw::hero::HeroSkill::LongDan:
                case tkw::hero::HeroSkill::QingGuo:
                    break;
                }
                return "发动 " +
                       std::string(tkw::hero::display_skill_name(req.hero_skill)) +
                       "（" + body + "）？";
            }

            std::string response_title(
                const tkw::game::ai::DecisionRequest &req, bool pair)
            {
                const bool jink =
                    req.response_kind == tkw::card::ResponseKind::Jink;
                const std::string need =
                    pair ? "需打出两张手牌当杀"
                         : (jink ? "需打出闪" : "需打出杀");
                if (req.response_source.empty())
                    return pair ? "响应（杀）：打出两张手牌当杀"
                                : "响应（" + need + "）";

                std::string title =
                    "响应（来源: " + req.response_user + " 的 " +
                    tkw::card::display_name(req.catalog, req.response_source) +
                    "，" + need;
                if (req.response_damage > 0)
                    title += "；不出将受到 " +
                             std::to_string(req.response_damage) + " 点伤害";
                title += "）";
                return title;
            }

            std::string reveal_title(
                const tkw::game::ai::DecisionRequest &req)
            {
                switch (req.reveal_source)
                {
                case tkw::game::RevealSource::Qilin:
                    return "麒麟弓：选择目标坐骑";
                case tkw::game::RevealSource::FireAttackReveal:
                    return "火攻：展示一张手牌";
                case tkw::game::RevealSource::FireAttackDiscard:
                    return "火攻：弃一张同花色手牌（可放弃）";
                case tkw::game::RevealSource::Wugu:
                    break;
                }
                return "五谷丰登亮牌（每名角色依次选一张）";
            }

            std::string counter_title(
                const tkw::game::ai::DecisionRequest &req)
            {
                std::string title = "无懈可击窗口";
                if (req.counter_user.empty())
                {
                    title += "（延时锦囊判定";
                    if (!req.counter_trick.empty())
                        title += "：" + tkw::card::display_name(
                                             req.catalog, req.counter_trick);
                    title += "）";
                }
                else
                {
                    title += "（使用者: " + req.counter_user;
                    if (!req.counter_trick.empty())
                        title += " 的 " + tkw::card::display_name(
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

            bool is_self_target_borrowed_sword(
                const tkw::game::ai::DecisionRequest &req,
                const tkw::game::LegalAction &act, std::string &holder)
            {
                if (!req.catalog || act.targets.size() != 2 ||
                    act.targets[1] != req.actor)
                    return false;

                const auto def = req.catalog->find(act.card.def_id);
                if (def.is_none() || def.unwrap()->effect.is_none())
                    return false;
                if (def.unwrap()->effect.unwrap().kind !=
                    tkw::card::CardEffectKind::BorrowedSword)
                    return false;

                holder = act.targets[0];
                return true;
            }

            std::string join_targets(
                const std::vector<std::string> &targets)
            {
                std::string out;
                for (std::size_t i = 0; i < targets.size(); ++i)
                {
                    if (i > 0)
                        out += ",";
                    out += targets[i];
                }
                return out;
            }

            const char *suit_glyph(tkw::card::Suit suit)
            {
                switch (suit)
                {
                case tkw::card::Suit::Spade:
                    return "♠";
                case tkw::card::Suit::Club:
                    return "♣";
                case tkw::card::Suit::Heart:
                    return "♥";
                case tkw::card::Suit::Diamond:
                    return "♦";
                }
                return "?";
            }

            std::string card_meta(const tkw::card::Card &c)
            {
                return std::string(suit_glyph(c.suit)) + std::to_string(c.number);
            }

            bool is_hidden_pick_option(
                const tkw::game::ai::DecisionRequest &req, std::size_t index)
            {
                if (req.kind != tkw::game::ai::DecisionKind::PickCard)
                    return false;
                if (index >= req.zone_labels.size())
                    return true;
                return req.zone_labels[index] == tkw::card::Zone::Hand;
            }

            void fill_card_strings(std::string &name, std::string &meta,
                                   std::string &text,
                                   const tkw::game::ai::DecisionRequest &req,
                                   const tkw::card::Card &c)
            {
                if (c.def_id.empty())
                    return;

                name = tkw::card::display_name(req.catalog, c.def_id);
                if (req.catalog)
                {
                    const auto def = req.catalog->find(c.def_id);
                    if (def.is_some())
                        text = def.unwrap()->text;
                }
                meta = card_meta(c);
            }

            void fill_card_fields(PanelOption &opt,
                                  const tkw::game::ai::DecisionRequest &req,
                                  const tkw::card::Card &c)
            {
                fill_card_strings(opt.card_name, opt.card_meta, opt.card_text, req,
                                  c);
            }

            void fill_second_card_fields(
                PanelOption &opt, const tkw::game::ai::DecisionRequest &req,
                const std::string &instance_id)
            {
                for (const auto &c : req.view.hand)
                    if (c.instance_id == instance_id)
                    {
                        fill_card_strings(opt.second_card_name,
                                          opt.second_card_meta,
                                          opt.second_card_text, req, c);
                        return;
                    }
            }

            std::string play_option_text(
                const tkw::game::ai::DecisionRequest &req,
                const tkw::game::LegalAction &act)
            {
                std::string text =
                    tkw::card::display_name(req.catalog, act.card.def_id) + " " +
                    act.card.instance_id;
                if (act.converted_sha)
                    text += "（当杀）";
                if (!act.second_instance_id.empty())
                    text += " + " + act.second_instance_id;
                if (!act.targets.empty())
                    text += " -> " + join_targets(act.targets);
                if (act.recast)
                    text += "（重铸：弃置并摸一张）";
                std::string holder;
                if (is_self_target_borrowed_sword(req, act, holder))
                    text += "（警告：" + holder +
                            " 将对你出杀，可能致你受伤或阵亡）";
                return text;
            }

            std::string pick_option_text(
                const tkw::game::ai::DecisionRequest &req, std::size_t index)
            {
                const tkw::card::Zone zone =
                    index < req.zone_labels.size() ? req.zone_labels[index]
                                                   : tkw::card::Zone::Hand;
                std::string text = zone_tag(zone);
                if (!text.empty())
                    text += " ";
                if (is_hidden_pick_option(req, index))
                {
                    text += kHiddenHandPlaceholder;
                    return text;
                }
                text += tkw::card::display_name(req.catalog, req.options[index].def_id) +
                        " " + req.options[index].instance_id;
                return text;
            }
        }  // namespace detail

        DecisionPanelView make_panel(
            const tkw::game::ai::DecisionRequest &req)
        {
            using tkw::game::ai::DecisionKind;

            DecisionPanelView panel;
            panel.kind = req.kind;
            panel.actor = req.actor;

            switch (req.kind)
            {
            case DecisionKind::Play:
            {
                panel.title = "出牌阶段";
                panel.allow_pass = true;
                for (const auto &act : req.legal)
                {
                    PanelOption opt;
                    opt.text = detail::play_option_text(req, act);
                    opt.instance_id = act.card.instance_id;
                    opt.second_instance_id = act.second_instance_id;
                    opt.targets = act.targets;
                    opt.recast = act.recast;
                    opt.converted_sha = act.converted_sha;
                    detail::fill_card_fields(opt, req, act.card);
                    if (!act.second_instance_id.empty())
                        detail::fill_second_card_fields(opt, req,
                                                        act.second_instance_id);
                    panel.options.push_back(std::move(opt));
                }
                break;
            }
            case DecisionKind::Response:
            {
                const bool pair = !req.legal.empty();
                panel.title = detail::response_title(req, pair);
                panel.allow_pass = true;
                if (pair)
                {
                    for (const auto &act : req.legal)
                    {
                        PanelOption opt;
                        opt.text =
                            tkw::card::display_name(req.catalog, act.card.def_id) +
                            " " + act.card.instance_id + " + " +
                            act.second_instance_id;
                        opt.instance_id = act.card.instance_id;
                        opt.second_instance_id = act.second_instance_id;
                        detail::fill_card_fields(opt, req, act.card);
                        detail::fill_second_card_fields(opt, req,
                                                        act.second_instance_id);
                        panel.options.push_back(std::move(opt));
                    }
                }
                else
                {
                    for (const auto &c : req.options)
                    {
                        PanelOption opt;
                        opt.text =
                            tkw::card::display_name(req.catalog, c.def_id) + " " +
                            c.instance_id;
                        opt.instance_id = c.instance_id;
                        detail::fill_card_fields(opt, req, c);
                        panel.options.push_back(std::move(opt));
                    }
                }
                break;
            }
            case DecisionKind::Peach:
            {
                panel.title = "濒死救场（濒死者: " + req.dying + "）";
                panel.allow_pass = true;
                for (const auto &c : req.options)
                {
                    PanelOption opt;
                    opt.text = tkw::card::display_name(req.catalog, c.def_id) +
                               " " + c.instance_id;
                    opt.instance_id = c.instance_id;
                    detail::fill_card_fields(opt, req, c);
                    panel.options.push_back(std::move(opt));
                }
                break;
            }
            case DecisionKind::Counter:
            {
                panel.title = detail::counter_title(req);
                panel.allow_pass = true;
                for (const auto &c : req.options)
                {
                    PanelOption opt;
                    opt.text = tkw::card::display_name(req.catalog, c.def_id) +
                               " " + c.instance_id;
                    opt.instance_id = c.instance_id;
                    detail::fill_card_fields(opt, req, c);
                    panel.options.push_back(std::move(opt));
                }
                break;
            }
            case DecisionKind::Trigger:
            {
                if (req.hero_trigger)
                    panel.title = detail::hero_trigger_title(req);
                else
                    panel.title = "发动 " + detail::ability_name(req) + "（" +
                                  detail::ability_hint(req.ability) + "）？";
                panel.allow_pass = true;
                panel.yes_no = true;
                PanelOption yes;
                yes.text = "发动";
                yes.accepted = true;
                panel.options.push_back(std::move(yes));
                PanelOption no;
                no.text = "不发动";
                no.accepted = false;
                panel.options.push_back(std::move(no));
                break;
            }
            case DecisionKind::PickCard:
            {
                panel.title = "选择目标区域的牌（目标: " + req.target + "）";
                panel.allow_pass = true;
                for (std::size_t i = 0; i < req.options.size(); ++i)
                {
                    PanelOption opt;
                    opt.text = detail::pick_option_text(req, i);
                    opt.instance_id = req.options[i].instance_id;
                    opt.option_index = i;
                    if (detail::is_hidden_pick_option(req, i))
                        opt.hidden = true;
                    else
                        detail::fill_card_fields(opt, req, req.options[i]);
                    panel.options.push_back(std::move(opt));
                }
                break;
            }
            case DecisionKind::PickRevealed:
            {
                panel.title = detail::reveal_title(req);
                panel.allow_pass =
                    req.reveal_source ==
                    tkw::game::RevealSource::FireAttackDiscard;
                for (std::size_t i = 0; i < req.options.size(); ++i)
                {
                    PanelOption opt;
                    opt.text =
                        tkw::card::display_name(req.catalog, req.options[i].def_id) +
                        " " + req.options[i].instance_id;
                    opt.instance_id = req.options[i].instance_id;
                    opt.option_index = i;
                    detail::fill_card_fields(opt, req, req.options[i]);
                    panel.options.push_back(std::move(opt));
                }
                break;
            }
            case DecisionKind::Discard:
            {
                panel.title =
                    "弃牌（" +
                    std::string(tkw::game::discard_reason_text(req.discard_reason)) +
                    "，需弃 " + std::to_string(req.count) + " 张）";
                panel.multi = true;
                panel.toggle = true;
                panel.need_count = req.count;
                panel.allow_pass =
                    req.discard_reason == tkw::game::DiscardReason::CixiongChoice;
                for (const auto &c : req.options)
                {
                    PanelOption opt;
                    opt.text = tkw::card::display_name(req.catalog, c.def_id) +
                               " " + c.instance_id;
                    opt.instance_id = c.instance_id;
                    detail::fill_card_fields(opt, req, c);
                    panel.options.push_back(std::move(opt));
                }
                break;
            }
            }
            return panel;
        }

        bool make_choice(const DecisionPanelView &panel,
                         const std::vector<std::size_t> &selected, bool pass,
                         tkw::game::ai::DecisionChoice &out)
        {
            using tkw::game::ai::DecisionKind;

            out = tkw::game::ai::DecisionChoice{};

            if (panel.kind == DecisionKind::Trigger)
            {
                if (pass)
                {
                    out.accepted = false;
                    return true;
                }
                if (selected.size() != 1 || selected[0] >= 2)
                    return false;
                out.accepted = selected[0] == 0;
                return true;
            }

            if (pass)
            {
                if (!panel.allow_pass)
                    return false;
                return true;  // 空选择：结束出牌/不响应/不救/不出无懈/放弃选牌
            }

            if (panel.kind == DecisionKind::Discard)
            {
                if (selected.size() !=
                    static_cast<std::size_t>(panel.need_count))
                    return false;
                std::vector<std::size_t> seen;
                for (const std::size_t i : selected)
                {
                    if (i >= panel.options.size())
                        return false;
                    if (std::find(seen.begin(), seen.end(), i) != seen.end())
                        return false;
                    seen.push_back(i);
                }
                for (const std::size_t i : selected)
                    out.discards.push_back(panel.options[i].instance_id);
                return true;
            }

            if (selected.size() != 1)
                return false;
            const std::size_t index = selected[0];
            if (index >= panel.options.size())
                return false;
            const PanelOption &opt = panel.options[index];

            switch (panel.kind)
            {
            case DecisionKind::Play:
                out.instance_id = tkw::Option<std::string>::Some(opt.instance_id);
                out.second_instance_id = opt.second_instance_id;
                out.targets = opt.targets;
                out.recast = opt.recast;
                out.converted_sha = opt.converted_sha;
                return true;
            case DecisionKind::Response:
                out.instance_id = tkw::Option<std::string>::Some(opt.instance_id);
                out.second_instance_id = opt.second_instance_id;
                return true;
            case DecisionKind::Peach:
            case DecisionKind::Counter:
                out.instance_id = tkw::Option<std::string>::Some(opt.instance_id);
                return true;
            case DecisionKind::PickCard:
            case DecisionKind::PickRevealed:
                out.option_index = tkw::Option<std::size_t>::Some(index);
                return true;
            default:
                return false;
            }
        }

        tkw::game::ai::DecisionChoice TuiDecisionSource::decide(
            const tkw::game::ai::DecisionRequest &req)
        {
            if (m_humans.count(req.actor) == 0)
                return m_fallback->decide(req);

            std::unique_lock<std::mutex> lock(m_mutex);
            if (m_cancelled.load())
                return {};
            m_panel = tkw::Option<DecisionPanelView>::Some(make_panel(req));
            m_taken = false;
            m_submitted = false;
            std::function<void()> notify = m_notify;
            std::function<void()> on_wait = m_on_wait;
            lock.unlock();
            // 先投递快照再发布待决：主线程只能在 pending 可见后写运行中
            // 提示，故该提示不会先于快照写入而被后续整段覆盖丢行。
            if (on_wait)
                on_wait();
            lock.lock();
            if (m_cancelled.load())
                return {};
            m_has_pending = true;
            lock.unlock();
            // notify 必须在 pending 可见之后，UI 唤醒后 fetch_new 才能取到面板。
            if (notify)
                notify();
            lock.lock();
            m_cv.wait(lock, [this]
                     { return m_submitted || m_cancelled.load(); });
            if (m_cancelled.load())
            {
                m_has_pending = false;
                return {};
            }
            m_has_pending = false;
            m_taken = false;
            return m_choice;
        }

        bool TuiDecisionSource::fetch_new(DecisionPanelView &out)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_has_pending || m_taken || m_cancelled.load())
                return false;
            out = m_panel.unwrap();
            m_taken = true;
            return true;
        }

        bool TuiDecisionSource::has_pending() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_has_pending && !m_taken && !m_cancelled.load();
        }

        bool TuiDecisionSource::submit(std::vector<std::size_t> selected,
                                       bool pass)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            // m_submitted 关掉「首次提交到 worker 唤醒之间」的连击覆盖窗口；
            // worker 每次 decide 起始会重置它。
            if (!m_has_pending || m_submitted || m_cancelled.load())
                return false;
            tkw::game::ai::DecisionChoice choice;
            if (m_panel.is_none() ||
                !make_choice(m_panel.unwrap(), selected, pass, choice))
                return false;
            m_choice = std::move(choice);
            m_submitted = true;
            m_cv.notify_all();
            return true;
        }

        void TuiDecisionSource::cancel()
        {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_cancelled.store(true);
                m_has_pending = false;
                m_panel = tkw::Option<DecisionPanelView>::None();
            }
            m_cv.notify_all();
        }

        void TuiDecisionSource::set_notify(std::function<void()> notify)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_notify = std::move(notify);
        }

        void TuiDecisionSource::set_on_wait(std::function<void()> on_wait)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_on_wait = std::move(on_wait);
        }
    }  // namespace tui
}  // namespace tkw
