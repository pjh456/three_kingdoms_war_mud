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
#include "game/core/roles.hpp"
#include "game/flow/table.hpp"
#include "save/session_meta.hpp"

namespace tkw
{
    namespace cli
    {
        namespace detail
        {
            /** 未实现卡展示名：目录中文名 + (id) 后缀；目录未收录时回落 id。 */
            inline std::string audit_entry_name(
                const tkw::card::CardDefCatalog &catalog, const std::string &def_id)
            {
                return tkw::card::display_name(catalog, def_id) + "(" + def_id + ")";
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
             * @brief 卡牌效果文案：数据源为 CardDef.text。
             * @param def 卡牌定义。
             * @return text 非空原样返回；空文案回落「（无说明）」占位，避免用户
             *         误以为命令漏输出。
             */
            inline std::string card_text_of(const tkw::card::CardDef &def)
            {
                return def.text.empty() ? "（无说明）" : def.text;
            }

            /** 卡牌区域 → 中文展示（事件日志移牌行用；Limbo 为正在转移）。 */
            inline constexpr const char *zone_name_zh(const tkw::Zone z)
            {
                switch (z)
                {
                case tkw::Zone::Draw:
                    return "摸牌堆";
                case tkw::Zone::Discard:
                    return "弃牌堆";
                case tkw::Zone::Hand:
                    return "手牌";
                case tkw::Zone::Equip:
                    return "装备区";
                case tkw::Zone::Judge:
                    return "判定区";
                default:
                    return "临时区";
                }
            }

            /**
             * @brief 订阅本局事件日志：verbose 为真时打印摸牌（含击杀奖励）/打牌/弃牌/移牌/伤害/体力/阵亡。
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
                                  << tkw::card::display_name(catalog, c.event.def_id)
                                  << "\n";
                    })));
                handles.push_back(
                    game.bus.subscribe(tkw::Handler<tkw::CardDiscardedEvent>(
                        [&catalog = game.catalog](
                            tkw::HandlerContext<tkw::CardDiscardedEvent> &c) {
                            // 判定翻牌/响应打出与真实弃置同走弃置事件，标签按来源语义区分。
                            const char *label = "[弃置] ";
                            if (c.event.kind == tkw::DiscardKind::Judgement)
                                label = "[判定] ";
                            else if (c.event.kind == tkw::DiscardKind::Response)
                                label = "[打出] ";
                            std::cout << label << c.event.entity << " "
                                      << tkw::card::display_name(
                                             catalog, c.event.def_id)
                                      << "\n";
                        })));
                handles.push_back(game.bus.subscribe(tkw::Handler<tkw::CardDrawnEvent>(
                    [&catalog = game.catalog](
                        tkw::HandlerContext<tkw::CardDrawnEvent> &c) {
                        // 击杀奖惩摸牌与常规摸牌同走摸牌事件，标签按来源语义区分。
                        const char *label =
                            c.event.kind == tkw::DrawKind::KillReward
                                ? "[击杀奖励] "
                                : "[摸牌] ";
                        std::cout << label << c.event.entity << " "
                                  << tkw::card::display_name(catalog, c.event.def_id)
                                  << "\n";
                    })));
                handles.push_back(
                    game.bus.subscribe(tkw::Handler<tkw::CardMovedEvent>(
                        [&catalog = game.catalog](
                            tkw::HandlerContext<tkw::CardMovedEvent> &c) {
                            // 空实体 = 亮牌等非玩家来源/去向，渲染为 (无) 避免空段。
                            std::string_view from = c.event.from_entity;
                            if (from.empty())
                                from = "(无)";
                            std::string_view to = c.event.to_entity;
                            if (to.empty())
                                to = "(无)";
                            std::cout << "[移牌] " << from << "("
                                      << zone_name_zh(c.event.from) << ") -> "
                                      << to << "(" << zone_name_zh(c.event.to)
                                      << ") "
                                      << tkw::card::display_name(catalog, c.event.def_id)
                                      << "\n";
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

            /** 身份局角色 → 中文展示；Role::None 回落「未知」。 */
            inline const char *role_label_zh(tkw::game::Role r)
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

            /**
             * @brief 身份局终局阵营标签。
             * @param camp 胜利阵营；Draw/None 回落同归于尽口径。
             * @param rep  阵营代表 id（可空）；反贼代表可能是已阵亡者，展示 id 会
             *             误导，故忽略。
             * @return 主公阵营胜/反贼阵营胜/内奸胜（带代表 id 时加括号）/平局。
             * @note 仅 identity 已结束会话调用；与乱斗 winner_label 的回落口径区分：
             *       本函数把空 rep 视为同归于尽平局，乱斗统计块的「无」不走这里。
             */
            inline std::string identity_result_label(
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

#endif  // INCLUDE_TKW_CLI_RENDER_HPP
