/**
 * @file   render.cpp
 * @brief  CLI 渲染层的定义。
 * @ingroup tkw_cli
 */

#include "cli/render.hpp"

#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace tkw
{
    namespace cli
    {
        namespace detail
        {
            std::string audit_entry_name(
                const tkw::card::CardDefCatalog &catalog, const std::string &def_id)
            {
                return tkw::card::display_name(catalog, def_id) + "(" + def_id + ")";
            }

            std::string card_text_of(const tkw::card::CardDef &def)
            {
                return def.text.empty() ? "（无说明）" : def.text;
            }

            std::vector<tkw::EventBus::Handle> subscribe_event_log(
                tkw::game::Game &game, bool verbose,
                const std::vector<std::string> &humans)
            {
                if (!verbose)
                    return {};
                return subscribe_event_log_to(
                    game,
                    [](const std::string &line) { std::cout << line << "\n"; },
                    [visible = std::set<std::string>(humans.begin(), humans.end())](
                        const std::string &entity) -> bool
                    { return visible.empty() || visible.count(entity) != 0; });
            }

            std::vector<tkw::EventBus::Handle> subscribe_stats(
                tkw::game::Game &game, tkw::save::BattleStats &stats)
            {
                std::vector<tkw::EventBus::Handle> handles;
                handles.push_back(
                    game.bus.subscribe(tkw::Handler<tkw::EntityDamagedEvent>(
                        [&stats](tkw::HandlerContext<tkw::EntityDamagedEvent> &c)
                        {
                            stats.last_hit_source[c.event.target] = c.event.source;
                            if (c.event.source.empty())
                                return;
                            stats.damage_dealt[c.event.source] += c.event.amount;
                        })));
                handles.push_back(game.bus.subscribe(tkw::Handler<tkw::EntityHealedEvent>(
                    [&stats](tkw::HandlerContext<tkw::EntityHealedEvent> &c)
                    { stats.healing[c.event.target] += c.event.amount; })));
                handles.push_back(
                    game.bus.subscribe(tkw::Handler<tkw::EntityDiedEvent>(
                        [&game, &stats](tkw::HandlerContext<tkw::EntityDiedEvent> &c)
                        {
                            stats.died.insert(c.event.entity_id);
                            const auto hit = stats.last_hit_source.find(c.event.entity_id);
                            if (hit == stats.last_hit_source.end())
                                return;
                            if (game.entities.find(hit->second).is_some())
                                ++stats.kills[hit->second];
                        })));
                return handles;
            }

            std::vector<std::string> battle_stats_lines(
                const tkw::save::BattleStats &stats, const tkw::game::Game &game,
                const std::string &winner, int turns)
            {
                const auto val = [](const std::map<std::string, int> &m,
                                    const std::string &k)
                {
                    const auto it = m.find(k);
                    return it == m.end() ? 0 : it->second;
                };
                std::vector<std::string> lines;
                lines.push_back("对局统计:");
                lines.push_back("  回合数: " + std::to_string(turns));
                lines.push_back("  胜者: " + (winner.empty() ? "无" : winner));
                std::set<std::string> ids = stats.died;
                for (const auto *e : game.entities.const_view())
                    ids.insert(e->get_id());
                for (const auto &id : ids)
                {
                    const auto alive = game.entities.find(id);
                    std::string hp;
                    if (alive.is_some())
                        hp = "体力 " + std::to_string(alive.unwrap()->get_hp()) +
                             "/" + std::to_string(alive.unwrap()->get_hp_bar().get_max());
                    else
                        hp = "阵亡";
                    lines.push_back("  " + id + ": " + hp + "，击杀 " +
                                    std::to_string(val(stats.kills, id)) +
                                    "，伤害 " +
                                    std::to_string(val(stats.damage_dealt, id)) +
                                    "，治疗 " +
                                    std::to_string(val(stats.healing, id)));
                }
                return lines;
            }

            void print_battle_stats(
                const tkw::save::BattleStats &stats, const tkw::game::Game &game,
                const std::string &winner, int turns)
            {
                for (const auto &line :
                     battle_stats_lines(stats, game, winner, turns))
                    std::cout << line << "\n";
            }

            std::string winner_label(const std::string &winner)
            {
                return winner.empty() ? "平局（同归于尽）" : winner;
            }

            const char *role_label_zh(tkw::game::Role r)
            {
                switch (r)
                {
                case tkw::game::Role::Lord:
                    return "主公";
                case tkw::game::Role::Loyalist:
                    return "忠臣";
                case tkw::game::Role::Rebel:
                    return "反贼";
                case tkw::game::Role::Traitor:
                    return "内奸";
                default:
                    return "未知";
                }
            }

            std::string identity_result_label(
                tkw::game::WinCamp camp, const std::string &rep)
            {
                switch (camp)
                {
                case tkw::game::WinCamp::LordCamp:
                    return rep.empty() ? "主公阵营胜" : "主公阵营胜（" + rep + "）";
                case tkw::game::WinCamp::TraitorCamp:
                    return rep.empty() ? "内奸胜" : "内奸胜（" + rep + "）";
                case tkw::game::WinCamp::RebelCamp:
                    return "反贼阵营胜";
                case tkw::game::WinCamp::Draw:
                default:
                    return "平局（同归于尽）";
                }
            }

        }  // namespace detail
    }  // namespace cli
}  // namespace tkw
