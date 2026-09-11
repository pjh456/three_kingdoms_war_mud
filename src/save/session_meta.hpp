/**
 * @file session_meta.hpp
 * @brief 存档可选的会话元数据：AI 难度档文本与对局统计。
 * @note 与 GameSession 分离：GameSession 只承载引擎进度，本结构由应用层填充；
 *       save 层不认 CLI 枚举，ai 以值域文本承载，未知文本由读取方回落默认。
 */

#ifndef INCLUDE_TKW_SAVE_SESSION_META_HPP
#define INCLUDE_TKW_SAVE_SESSION_META_HPP

#include <map>
#include <set>
#include <string>

namespace tkw
{
    namespace save
    {
        /** 对局统计聚合：按已发布事件累计伤害/治疗与击杀归属，供复盘打印。 */
        struct BattleStats
        {
            std::map<std::string, int> damage_dealt;            /**< 来源 → 造成伤害总量 */
            std::map<std::string, int> healing;                 /**< 目标 → 恢复总量 */
            std::map<std::string, int> kills;                   /**< 击杀者 → 击杀数 */
            std::map<std::string, std::string> last_hit_source; /**< 受害者 → 最近伤害来源 */
            std::set<std::string> died;                         /**< 本局阵亡实体 id */
        };

        /** 存档伴随的会话元数据；ai 空 / stats 全空表示未写，读档回落默认。 */
        struct SessionMeta
        {
            std::string ai;    /**< AI 难度档文本（"simple"/"aggressive"；空 = 未写） */
            BattleStats stats; /**< 对局统计；全空 = 未写 */
        };
    }
}

#endif  // INCLUDE_TKW_SAVE_SESSION_META_HPP
