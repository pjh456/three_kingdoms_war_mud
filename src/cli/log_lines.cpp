/**
 * @file   log_lines.cpp
 * @brief  CLI 事件日志纯格式化与订阅的定义。
 * @ingroup tkw_cli
 */

#include "cli/log_lines.hpp"

#include <string>
#include <vector>

namespace tkw
{
    namespace cli
    {
        namespace detail
        {
            std::string card_played_line(
                const tkw::card::CardDefCatalog &catalog,
                const tkw::CardPlayedEvent &event)
            {
                return "[打出] " + event.user + " " +
                       tkw::card::display_name(catalog, event.def_id);
            }

            std::string card_discarded_line(
                const tkw::card::CardDefCatalog &catalog,
                const tkw::CardDiscardedEvent &event)
            {
                const char *label = "[弃置] ";
                if (event.kind == tkw::DiscardKind::Judgement)
                    label = "[判定] ";
                else if (event.kind == tkw::DiscardKind::Response)
                    label = "[打出] ";
                const std::string entity =
                    event.entity.empty() ? "(无)" : event.entity;
                return std::string(label) + entity + " " +
                       tkw::card::display_name(catalog, event.def_id);
            }

            std::string card_drawn_line(
                const tkw::card::CardDefCatalog &catalog,
                const tkw::CardDrawnEvent &event, bool reveal)
            {
                const char *label =
                    event.kind == tkw::DrawKind::KillReward ? "[击杀奖励] "
                                                            : "[摸牌] ";
                const std::string name =
                    reveal ? tkw::card::display_name(catalog, event.def_id)
                           : kHiddenCardName;
                return std::string(label) + event.entity + " " + name;
            }

            std::string card_moved_line(
                const tkw::card::CardDefCatalog &catalog,
                const tkw::CardMovedEvent &event)
            {
                const std::string from =
                    event.from_entity.empty() ? "(无)" : event.from_entity;
                const std::string to =
                    event.to_entity.empty() ? "(无)" : event.to_entity;
                return "[移牌] " + from + "(" + zone_name_zh(event.from) +
                       ") -> " + to + "(" + zone_name_zh(event.to) + ") " +
                       tkw::card::display_name(catalog, event.def_id);
            }

            const char *damage_type_hint_zh(tkw::card::DamageType type)
            {
                switch (type)
                {
                case tkw::card::DamageType::Fire:
                    return "火";
                case tkw::card::DamageType::Thunder:
                    return "雷";
                case tkw::card::DamageType::Normal:
                    return "";
                }
                return "";
            }

            std::string entity_damaged_line(
                const tkw::EntityDamagedEvent &event)
            {
                const std::string source =
                    event.source.empty() ? "(无来源)" : event.source;
                std::string line = "[伤害] " + source + " -> " + event.target +
                                   " " + std::to_string(event.amount);

                std::string marks = damage_type_hint_zh(event.damage_type);
                if (event.indirect)
                {
                    if (!marks.empty())
                        marks += "，";
                    marks += "传导";
                }
                if (!marks.empty())
                    line += "（" + marks + "）";
                return line;
            }

            std::string entity_hp_changed_line(
                const tkw::EntityHpChangedEvent &event)
            {
                return "[体力] " + event.entity_id + " " +
                       std::to_string(event.old_cur) + "->" +
                       std::to_string(event.new_cur) + "/" +
                       std::to_string(event.max);
            }

            std::string entity_died_line(const tkw::EntityDiedEvent &event)
            {
                return "[阵亡] " + event.entity_id;
            }

        }  // namespace detail
    }  // namespace cli
}  // namespace tkw
