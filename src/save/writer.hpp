/**
 * @file   writer.hpp
 * @brief  对局状态 → 存档 JSON 文本。
 * @details 手工拼装 JSON（格式完全受控，字符串统一转义）；读取走 pjh_json。
 * @ingroup tkw_save
 */

#ifndef INCLUDE_TKW_SAVE_WRITER_HPP
#define INCLUDE_TKW_SAVE_WRITER_HPP

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "card/card.hpp"
#include "card/catalog.hpp"
#include "card/def.hpp"
#include "entity/manager.hpp"
#include "game/core/roles.hpp"
#include "game/core/rules.hpp"
#include "game/flow/loop.hpp"
#include "game/flow/table.hpp"
#include "save/deck_hash.hpp"
#include "save/format.hpp"
#include "save/session_meta.hpp"

namespace tkw
{
    namespace save
    {

        /**
         * @brief  JSON 字符串转义。
         * @param[in] s 原始字符串。
         * @return 转义 `"`、`\\` 与控制字符后的字符串。
         */
        std::string escape_json(std::string_view s);

        /**
         * @brief  将字符串序列化为带引号的 JSON 字符串字面量。
         * @param[in] s 原始字符串。
         * @return `"..."`，内部已转义。
         */
        std::string jstr(std::string_view s);

        /**
         * @brief  将单张牌序列化为 JSON 对象。
         * @param[in] c 待序列化的牌。
         * @return 含 `iid`/`def`/`suit`/`number` 的 JSON 对象文本。
         */
        std::string card_json(const card::Card &c);

        /**
         * @brief  将牌列表序列化为 JSON 数组。
         * @param[in] cards 待序列化的牌列表；空列表输出 `[]`。
         * @return JSON 数组文本，元素顺序与 `cards` 一致。
         */
        std::string cards_json(const std::vector<card::Card> &cards);

        namespace detail
        {
            /**
             * @brief  序列化 string→int 映射。
             * @param[in] m 待序列化的映射；`std::map` 迭代有序，输出确定。
             * @return JSON 对象文本。
             */
            std::string int_map_json(const std::map<std::string, int> &m);

            /**
             * @brief  序列化 string→string 映射。
             * @param[in] m 待序列化的映射；`std::map` 迭代有序，输出确定。
             * @return JSON 对象文本。
             */
            std::string str_map_json(
                const std::map<std::string, std::string> &m);

            /**
             * @brief  序列化 id→`Role` 映射。
             * @param[in] m 待序列化的角色表；`std::map` 迭代有序，输出确定。
             * @return JSON 对象文本，值为角色文本。
             */
            std::string role_map_json(const game::RoleTable &m);

            /**
             * @brief  序列化 string 集合为 JSON 数组。
             * @param[in] s 待序列化的集合；`std::set` 迭代有序，输出确定。
             * @return JSON 数组文本。
             */
            std::string str_set_json(const std::set<std::string> &s);

            /**
             * @brief  统计是否有任一非空字段。
             * @param[in] stats 待检查的统计聚合。
             * @return 是否有字段非空；全空表示未写，序列化时省略以保持规范往返。
             */
            bool has_any_stats(const BattleStats &stats);
        }

        /**
         * @brief  序列化整局（含会话进度与可选元数据）到 JSON 文本。
         * @details 按实际用到的最高格式特性动态写出版本号：有武将写
         *          `kVersionHeroes`，仅有连环写 `kVersionChained`，否则写
         *          `kVersion`。牌表指纹按 int64 位型写出，高位指纹落成负十进制，
         *          读取端逐位还原。
         * @param[in] g         对局运行时；只读，本接口不修改。
         * @param[in] session   会话进度。
         * @param[in] deck_name 牌表名称，写入 `deck.name`。
         * @param[in] meta      会话元数据；ai 空、stats 全空时不写对应字段，保证
         *                      默认元数据的规范化往返结果不因新增字段而变化。
         * @return 完整、自包含的存档 JSON 文本。
         * @post  本接口不改变任何状态；落盘原子性由调用方负责。
         * @note  身份局才写出 `mode` 与 `roles`；乱斗不写新键，保持旧档逐字节不变。
         * @see   save::read
         */
        std::string write(
            const game::Game &g, const game::GameSession &session,
            const std::string &deck_name, const SessionMeta &meta = {});
    }
}

#endif  // INCLUDE_TKW_SAVE_WRITER_HPP
