/**
 * @file render.hpp
 * @brief CLI 渲染层：卡名/类型展示、事件日志与对局统计的订阅和打印。
 * @note 只读展示，不改变对局状态；订阅句柄由调用方在命令作用域内持有并析构，
 *       不得存入会话（会话被覆盖时会先析构旧 Game 与总线）。
 */
#ifndef INCLUDE_TKW_CLI_RENDER_HPP
#define INCLUDE_TKW_CLI_RENDER_HPP

#include <iostream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "card/catalog.hpp"
#include "card/def.hpp"
#include "entity/event.hpp"
#include "event/event_bus.hpp"
#include "event/handler.hpp"
#include "game/core/card_event.hpp"
#include "game/flow/table.hpp"
#include "save/session_meta.hpp"

namespace tkw
{
    namespace cli
    {
        namespace detail
        {
            /** 卡牌 id → 目录中文名；目录未收录该 id 或名称为空时回落 id 本身。 */
            inline const std::string &card_name(
                const tkw::card::CardDefCatalog &catalog, const std::string &def_id)
            {
                const auto def = catalog.find(def_id);
                if (def.is_some() && !def.unwrap()->name.empty())
                    return def.unwrap()->name;
                return def_id;
            }

            /** 未实现卡展示名：目录中文名 + (id) 后缀；目录未收录时回落 id。 */
            inline std::string audit_entry_name(
                const tkw::card::CardDefCatalog &catalog, const std::string &def_id)
            {
                return card_name(catalog, def_id) + "(" + def_id + ")";
            }

            /** 卡牌大类 → 中文展示（基本/锦囊/装备）。 */
            inline constexpr const char *card_type_zh(const tkw::card::CardType t)
            {
                switch (t)
                {
                case tkw::card::CardType::Basic:
                    return "基本";
                case tkw::card::CardType::Trick:
                    return "锦囊";
                default:
                    return "装备";
                }
            }

            /**
             * @brief 订阅本局事件日志：verbose 为真时打印摸牌/打牌/弃牌/伤害/体力/阵亡。
             * @return 订阅句柄；verbose 为假时为空，句柄析构即退订。
             * @note 句柄只应活在需要日志的命令作用域内，不得存入 Session：会话被覆盖
             *       时会先析构旧 Game（含总线），遗留句柄将对已释放总线退订。
             */
            inline std::vector<tkw::EventBus::Handle> subscribe_event_log(
                tkw::game::Game &game, bool verbose)
            {
                std::vector<tkw::EventBus::Handle> handles;
                if (!verbose)
                    return handles;
                handles.push_back(game.bus.subscribe(tkw::Handler<tkw::CardPlayedEvent>(
                    [&catalog = game.catalog](tkw::HandlerContext<tkw::CardPlayedEvent> &c)
                    {
                        std::cout << "[打出] " << c.event.user << " "
                                  << card_name(catalog, c.event.def_id) << "\n";
                    })));
                handles.push_back(
                    game.bus.subscribe(tkw::Handler<tkw::CardDiscardedEvent>(
                        [&catalog = game.catalog](
                            tkw::HandlerContext<tkw::CardDiscardedEvent> &c) {
                            std::cout << "[弃置] " << c.event.entity << " "
                                      << card_name(catalog, c.event.def_id) << "\n";
                        })));
                handles.push_back(game.bus.subscribe(tkw::Handler<tkw::CardDrawnEvent>(
                    [&catalog = game.catalog](
                        tkw::HandlerContext<tkw::CardDrawnEvent> &c) {
                        std::cout << "[摸牌] " << c.event.entity << " "
                                  << card_name(catalog, c.event.def_id) << "\n";
                    })));
                handles.push_back(
                    game.bus.subscribe(tkw::Handler<tkw::EntityDamagedEvent>(
                        [](tkw::HandlerContext<tkw::EntityDamagedEvent> &c) {
                            // 无来源 = 闪电等非玩家来源，渲染为 (无来源) 避免空段。
                            std::string_view src = c.event.source;
                            if (src.empty())
                                src = "(无来源)";
                            std::cout << "[伤害] " << src << " -> "
                                      << c.event.target << " " << c.event.amount
                                      << "\n";
                        })));
                handles.push_back(
                    game.bus.subscribe(tkw::Handler<tkw::EntityHpChangedEvent>(
                        [](tkw::HandlerContext<tkw::EntityHpChangedEvent> &c) {
                            std::cout << "[体力] " << c.event.entity_id << " "
                                      << c.event.old_cur << "->" << c.event.new_cur
                                      << "/" << c.event.max << "\n";
                        })));
                handles.push_back(
                    game.bus.subscribe(tkw::Handler<tkw::EntityDiedEvent>(
                        [](tkw::HandlerContext<tkw::EntityDiedEvent> &c)
                        { std::cout << "[阵亡] " << c.event.entity_id << "\n"; })));
                return handles;
            }

            /**
             * @brief 订阅对局统计事件：把伤害/治疗/死亡累计入 stats（击杀按最近伤害来源归因）。
             * @param game  本局运行时；死亡事件处理中按存活实体判定击杀归属。
             * @param stats 聚合目标；句柄析构即退订，stats 须比句柄存活更久。
             * @return 订阅句柄集合。
             * @note 死亡事件不带击杀者字段，归因取「受害者最近一次伤害来源」；来源为空
             *       （闪电等）或已阵亡（同归于尽时先死者）不计击杀，与引擎击杀奖励口径一致。
             */
            inline std::vector<tkw::EventBus::Handle> subscribe_stats(
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

            /**
             * @brief 打印对局统计块（对局结束时调用，追加在胜者/平局行之后）。
             * @param stats  本局累计的统计聚合。
             * @param game   本局运行时；存活实体体力在此读取（阵亡者显示「阵亡」）。
             * @param winner 胜者 id；空串显示「无」（平局/同归于尽）。
             * @param turns  已执行回合数。
             * @note 玩家清单 = 存活实体 ∪ 阵亡记录，按 id 排序输出；统计以已发布事件
             *       为准，含存档恢复的部分与读档后新增的事件。
             */
            inline void print_battle_stats(
                const tkw::save::BattleStats &stats, const tkw::game::Game &game,
                const std::string &winner, int turns)
            {
                const auto val = [](const std::map<std::string, int> &m,
                                    const std::string &k)
                {
                    const auto it = m.find(k);
                    return it == m.end() ? 0 : it->second;
                };
                std::cout << "对局统计:\n";
                std::cout << "  回合数: " << turns << "\n";
                std::cout << "  胜者: " << (winner.empty() ? "无" : winner) << "\n";
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
                    std::cout << "  " << id << ": " << hp << "，击杀 "
                              << val(stats.kills, id) << "，伤害 "
                              << val(stats.damage_dealt, id) << "，治疗 "
                              << val(stats.healing, id) << "\n";
                }
            }

            /**
             * @brief 终结行胜者展示：会话已无存活者（空串）时回落「平局（同归于尽）」。
             * @note 只用于会话已结束（存活 ≤ 1）的胜者行：唯一存活者时胜者非空，
             *       回合上限平局走错误分支「平局（达到最大回合数）」，不经此处，
             *       故空串只可能来自同归于尽。统计块的「无」回落口径不变。
             */
            inline std::string winner_label(const std::string &winner)
            {
                return winner.empty() ? "平局（同归于尽）" : winner;
            }
        }  // namespace detail
    }  // namespace cli
}  // namespace tkw

#endif  // INCLUDE_TKW_CLI_RENDER_HPP
