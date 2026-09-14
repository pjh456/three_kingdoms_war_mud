/**
 * @file   render.hpp
 * @brief  CLI 渲染层。
 * @details 卡名/类型展示、事件日志与对局统计的订阅和打印；只读展示，不改变对局
 *          状态。
 * @note   订阅句柄由调用方在命令作用域内持有并析构，不得存入会话（会话被覆盖时
 *         会先析构旧 Game 与总线）。
 * @ingroup tkw_cli
 */
#ifndef INCLUDE_TKW_CLI_RENDER_HPP
#define INCLUDE_TKW_CLI_RENDER_HPP

#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "card/catalog.hpp"
#include "card/def.hpp"
#include "cli/log_lines.hpp"
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
            /** @brief 横置（连环）状态标签：CLI status 与 TUI 棋盘共用，防两侧文案漂移。 */
            inline constexpr const char *kChainedTag = "[横置]";

            /**
             * @brief  未实现卡展示名。
             * @param[in] catalog 牌表目录，用于解析中文名。
             * @param[in] def_id  卡牌 id。
             * @return 「目录中文名 + (id)」；目录未收录时回落 id。
             */
            std::string audit_entry_name(
                const tkw::card::CardDefCatalog &catalog, const std::string &def_id);

            /**
             * @brief  卡牌大类 → 中文展示。
             * @param[in] t 卡牌大类。
             * @return 基本/锦囊/装备；未知值回落「装备」。
             */
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
             * @param[in] def 卡牌定义。
             * @return text 非空原样返回；空文案回落「（无说明）」占位，避免用户
             *         误以为命令漏输出。
             */
            std::string card_text_of(const tkw::card::CardDef &def);

            /**
             * @brief 订阅本局事件日志：verbose 为真时打印摸牌（含击杀奖励）/打牌/弃牌/移牌/伤害/体力/阵亡。
             * @param[in] game    当前对局运行时；订阅其事件总线。
             * @param[in] verbose 是否打印事件日志；为假时返回空句柄集合。
             * @param[in] humans  本会话真人座位；非空时仅这些座位的摸牌渲染牌名，其余
             *                    渲染占位 `kHiddenCardName`，避免向操作者泄漏对手手牌；
             *                    空 = 无真人视角，全部渲染牌名。
             * @return 订阅句柄；verbose 为假时为空，句柄析构即退订。
             * @note 句柄只应活在需要日志的命令作用域内，不得存入 Session：会话被覆盖
             *       时会先析构旧 Game（含总线），遗留句柄将对已释放总线退订。
             */
            std::vector<tkw::EventBus::Handle> subscribe_event_log(
                tkw::game::Game &game, bool verbose,
                const std::vector<std::string> &humans = {});

            /**
             * @brief 订阅对局统计事件：把伤害/治疗/死亡累计入 stats（击杀按最近伤害来源归因）。
             * @param[in]     game  本局运行时；死亡事件处理中按存活实体判定击杀归属。
             * @param[in,out] stats 聚合目标；句柄析构即退订，stats 须比句柄存活更久。
             * @return 订阅句柄集合。
             * @note 死亡事件不带击杀者字段，归因取「受害者最近一次伤害来源」；来源为空
             *       （闪电等）或已阵亡（同归于尽时先死者）不计击杀，与引擎击杀奖励口径一致。
             */
            std::vector<tkw::EventBus::Handle> subscribe_stats(
                tkw::game::Game &game, tkw::save::BattleStats &stats);

            /**
             * @brief 对局统计块纯行（不含换行）：头行 + 回合/胜者 + 逐座体力/击杀/伤害/治疗。
             * @param[in] stats  本局累计的统计聚合。
             * @param[in] game   本局运行时；存活实体体力在此读取（阵亡者显示「阵亡」）。
             * @param[in] winner 胜者 id；空串显示「无」（平局/同归于尽）。
             * @param[in] turns  已执行回合数。
             * @return 行序：`对局统计:`、`  回合数: N`、`  胜者: X`，随后每玩家
             *         「  <id>: 体力 x/y，击杀 n，伤害 d，治疗 h」（阵亡者体力段为
             *         「阵亡」）。
             * @note 纯行构造无输出副作用：CLI 逐行打印与 TUI 逐行写日志共用，
             *       保证两处统计块逐字一致。玩家清单 = 存活实体 ∪ 阵亡记录，按
             *       id 排序输出；统计以已发布事件为准，含存档恢复的部分。
             */
            std::vector<std::string> battle_stats_lines(
                const tkw::save::BattleStats &stats, const tkw::game::Game &game,
                const std::string &winner, int turns);

            /**
             * @brief 打印对局统计块（对局结束时调用，追加在胜者/平局行之后）。
             * @param[in] stats  本局累计的统计聚合。
             * @param[in] game   本局运行时；存活实体体力在此读取（阵亡者显示「阵亡」）。
             * @param[in] winner 胜者 id；空串显示「无」（平局/同归于尽）。
             * @param[in] turns  已执行回合数。
             * @note 逐行打印 battle_stats_lines 的纯行，输出与既有 CLI 逐字节一致。
             */
            void print_battle_stats(
                const tkw::save::BattleStats &stats, const tkw::game::Game &game,
                const std::string &winner, int turns);

            /**
             * @brief 终结行胜者展示：会话已无存活者（空串）时回落「平局（同归于尽）」。
             * @param[in] winner 胜者 id；空串表示同归于尽。
             * @return 胜者 id，或「平局（同归于尽）」。
             * @note 只用于会话已结束（存活 ≤ 1）的胜者行：唯一存活者时胜者非空，
             *       回合上限平局走错误分支「平局（达到最大回合数）」，不经此处，
             *       故空串只可能来自同归于尽。统计块的「无」回落口径不变。
             */
            std::string winner_label(const std::string &winner);

            /**
             * @brief 身份局角色 → 中文展示。
             * @param[in] r 角色；`Role::None` 回落「未知」。
             * @return 角色中文名。
             * @note Role::None 回落「未知」；该文案亦作真人局未公开座位的隐藏
             *       占位（身份局运行中不存在真实无角色）。
             */
            const char *role_label_zh(tkw::game::Role r);

            /**
             * @brief 身份局终局阵营标签。
             * @param[in] camp 胜利阵营；Draw/None 回落同归于尽口径。
             * @param[in] rep  阵营代表 id（可空）；反贼代表可能是已阵亡者，展示 id 会
             *             误导，故忽略。
             * @return 主公阵营胜/反贼阵营胜/内奸胜（带代表 id 时加括号）/平局。
             * @note 仅 identity 已结束会话调用；与乱斗 winner_label 的回落口径区分：
             *       本函数把空 rep 视为同归于尽平局，乱斗统计块的「无」不走这里。
             */
            std::string identity_result_label(
                tkw::game::WinCamp camp, const std::string &rep);
        }  // namespace detail
    }  // namespace cli
}  // namespace tkw

#endif  // INCLUDE_TKW_CLI_RENDER_HPP
