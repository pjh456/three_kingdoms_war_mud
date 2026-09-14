/**
 * @file   log_lines.hpp
 * @brief  CLI 事件日志纯格式化。
 * @details 7 类事件 → 不含换行的整行文案 + 订阅单点；纯函数、不含 `<iostream>`，
 *          换行与输出流由调用方决定，CLI 与 TUI 共用同一事实源；空实体/空来源
 *          回落逻辑（(无)/(无来源)）留在格式化器内。
 * @ingroup tkw_cli
 */
#ifndef INCLUDE_TKW_CLI_LOG_LINES_HPP
#define INCLUDE_TKW_CLI_LOG_LINES_HPP

#include <string>
#include <vector>

#include "card/catalog.hpp"
#include "card/def.hpp"
#include "entity/event.hpp"
#include "event/event_bus.hpp"
#include "event/handler.hpp"
#include "game/core/card_event.hpp"
#include "game/flow/table.hpp"

namespace tkw
{
    namespace cli
    {
        namespace detail
        {
            /**
             * @brief  卡牌区域 → 中文展示。
             * @param[in] z 卡牌区域；`Limbo`（正在转移）回落「临时区」。
             * @return 区域的中文名。
             * @note   事件日志移牌行使用。
             */
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

            /** @brief 隐藏牌展示占位：不可见实体摸到的牌不向人类可见日志暴露牌名。 */
            inline constexpr const char *kHiddenCardName = "未知牌";

            /**
             * @brief [打出] 行：使用者在某结算点打出一张牌。
             * @param[in] catalog 牌表目录，用于解析展示名。
             * @param[in] event   打出事件。
             * @return 不含换行的整行文案。
             */
            inline std::string card_played_line(
                const tkw::card::CardDefCatalog &catalog,
                const tkw::CardPlayedEvent &event)
            {
                return "[打出] " + event.user + " " +
                       tkw::card::display_name(catalog, event.def_id);
            }

            /**
             * @brief [弃置] 行：进弃牌堆的标签按来源语义区分。
             * @param[in] catalog 牌表目录，用于解析展示名。
             * @param[in] event   弃置事件。
             * @return 不含换行的整行文案。
             * @note Judgement → [判定]、Response → [打出]，其余 → [弃置]；
             *       空 entity（无主/亮牌来源）渲染为 (无)，避免空段。
             */
            inline std::string card_discarded_line(
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

            /**
             * @brief [摸牌] 行：击杀奖惩摸牌与常规摸牌同走摸牌事件。
             * @param[in] catalog 牌表目录，用于解析展示名。
             * @param[in] event   摸牌事件。
             * @param[in] reveal  为假时牌名回落 `kHiddenCardName`，不向人类可见日志
             *                    暴露不可见实体的手牌；无真人视角的调用方保持默认全可见。
             * @return 不含换行的整行文案。
             * @note KillReward → [击杀奖励]，其余 → [摸牌]；行结构（标签 + 实体 +
             *       空格 + 名称）不变，便于既有解析。
             */
            inline std::string card_drawn_line(
                const tkw::card::CardDefCatalog &catalog,
                const tkw::CardDrawnEvent &event, bool reveal = true)
            {
                const char *label =
                    event.kind == tkw::DrawKind::KillReward ? "[击杀奖励] "
                                                            : "[摸牌] ";
                const std::string name =
                    reveal ? tkw::card::display_name(catalog, event.def_id)
                           : kHiddenCardName;
                return std::string(label) + event.entity + " " + name;
            }

            /**
             * @brief [移牌] 行：区域转移；空实体 = 亮牌等非玩家来源/去向。
             * @param[in] catalog 牌表目录，用于解析展示名。
             * @param[in] event   移牌事件。
             * @return 不含换行的整行文案。
             * @note 空实体渲染为 (无)，避免空段。
             */
            inline std::string card_moved_line(
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

            /**
             * @brief 伤害属性的中文单字标记；普通伤害返回空串。
             * @param[in] type 伤害属性。
             * @return 火→"火"、雷→"雷"、普通→空串。
             * @note 属性为「无」时不产生标记，避免普通伤害行出现冗余后缀。
             */
            inline const char *damage_type_hint_zh(tkw::card::DamageType type)
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

            /**
             * @brief [伤害] 行：无来源 = 闪电等非玩家来源。
             * @param[in] event 伤害事件。
             * @return 不含换行的整行文案。
             * @note 空来源渲染为 (无来源)，避免空段。普通伤害保持原格式逐字节
             *       不变；火/雷属性与连环等间接传导在整行尾部以全角括号标记，
             *       复合标记按属性在前、传导在后并以全角逗号分隔。
             */
            inline std::string entity_damaged_line(
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

            /**
             * @brief  [体力] 行：实体体力变化（旧->新/上限）。
             * @param[in] event 体力变化事件。
             * @return 不含换行的整行文案。
             */
            inline std::string entity_hp_changed_line(
                const tkw::EntityHpChangedEvent &event)
            {
                return "[体力] " + event.entity_id + " " +
                       std::to_string(event.old_cur) + "->" +
                       std::to_string(event.new_cur) + "/" +
                       std::to_string(event.max);
            }

            /**
             * @brief  [阵亡] 行：实体死亡（救场窗口关闭、无人救回）。
             * @param[in] event 阵亡事件。
             * @return 不含换行的整行文案。
             */
            inline std::string entity_died_line(const tkw::EntityDiedEvent &event)
            {
                return "[阵亡] " + event.entity_id;
            }

            /**
             * @brief 摸牌可见性谓词：默认全可见。
             * @note  无真人视角的 TUI / 全 AI 口径。
             */
            struct RevealAll
            {
                /**
                 * @brief  恒判定为可见（入参忽略）。
                 * @return 恒为 `true`。
                 */
                bool operator()(const std::string &) const noexcept { return true; }
            };

            /**
             * @brief 订阅本局 7 类日志事件，逐事件把整行文案交给 sink。
             * @param[in]  game   当前对局运行时；订阅其事件总线。
             * @param[in]  sink   接收整行文案的可调用对象。
             * @param[in]  reveal 决定某实体摸牌的牌名是否进入日志；默认全可见。
             * @tparam Sink 可拷贝可调用对象，签名兼容 `void(const std::string&)`。
             * @tparam Reveal 摸牌可见性谓词，签名兼容 `bool(const std::string&)`，
             *                入参为摸牌实体 id；返回假时该行牌名回落占位。
             * @return 订阅句柄；析构即退订。
             * @note 谓词按值捕获进摸牌 handler，随 handler 存活；其余 6 类事件为
             *       公开信息，不消费谓词。句柄只应活在需要日志的作用域内，不得存入
             *       会话：会话被覆盖时会先析构旧 Game（含总线），遗留句柄将对已
             *       释放总线退订。
             */
            template <typename Sink, typename Reveal = RevealAll>
            inline std::vector<tkw::EventBus::Handle> subscribe_event_log_to(
                tkw::game::Game &game, Sink sink, Reveal reveal = {})
            {
                std::vector<tkw::EventBus::Handle> handles;
                handles.push_back(game.bus.subscribe(tkw::Handler<tkw::CardPlayedEvent>(
                    [&catalog = game.catalog, sink](
                        tkw::HandlerContext<tkw::CardPlayedEvent> &c)
                    { sink(card_played_line(catalog, c.event)); })));
                handles.push_back(game.bus.subscribe(
                    tkw::Handler<tkw::CardDiscardedEvent>(
                        [&catalog = game.catalog, sink](
                            tkw::HandlerContext<tkw::CardDiscardedEvent> &c)
                        { sink(card_discarded_line(catalog, c.event)); })));
                handles.push_back(game.bus.subscribe(
                    tkw::Handler<tkw::CardDrawnEvent>(
                        [&catalog = game.catalog, sink, reveal](
                            tkw::HandlerContext<tkw::CardDrawnEvent> &c)
                        {
                            sink(card_drawn_line(
                                catalog, c.event, reveal(c.event.entity)));
                        })));
                handles.push_back(game.bus.subscribe(
                    tkw::Handler<tkw::CardMovedEvent>(
                        [&catalog = game.catalog, sink](
                            tkw::HandlerContext<tkw::CardMovedEvent> &c)
                        { sink(card_moved_line(catalog, c.event)); })));
                handles.push_back(game.bus.subscribe(
                    tkw::Handler<tkw::EntityDamagedEvent>(
                        [sink](tkw::HandlerContext<tkw::EntityDamagedEvent> &c)
                        { sink(entity_damaged_line(c.event)); })));
                handles.push_back(game.bus.subscribe(
                    tkw::Handler<tkw::EntityHpChangedEvent>(
                        [sink](tkw::HandlerContext<tkw::EntityHpChangedEvent> &c)
                        { sink(entity_hp_changed_line(c.event)); })));
                handles.push_back(game.bus.subscribe(
                    tkw::Handler<tkw::EntityDiedEvent>(
                        [sink](tkw::HandlerContext<tkw::EntityDiedEvent> &c)
                        { sink(entity_died_line(c.event)); })));
                return handles;
            }
        }  // namespace detail
    }  // namespace cli
}  // namespace tkw

#endif  // INCLUDE_TKW_CLI_LOG_LINES_HPP
