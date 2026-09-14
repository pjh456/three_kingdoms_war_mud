/**
 * @file factory.hpp
 * @brief 建局装配：加载牌堆与武将目录、建 `Game`、逐座创建玩家。
 * @details 装配顺序：身份局人数校验 → 加载牌堆目录 → 加载武将目录（可选）→
 *          构造 `Game`（注入 `SeededRng`）→ 逐座创建玩家 → 身份局洗牌分配角色。
 *          game 层建局入口，不依赖 CLI 类型；hand/AI/verbose 等会话参数不参与
 *          装配，由调用方在开局准备（`start_session`）与决策源构造处消费。
 * @ingroup tkw_game_flow
 */

#ifndef INCLUDE_TKW_GAME_FACTORY_HPP
#define INCLUDE_TKW_GAME_FACTORY_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "card/catalog.hpp"
#include "config/error.hpp"
#include "config/resource.hpp"
#include "entity/base.hpp"
#include "entity/hp.hpp"
#include "game/core/roles.hpp"
#include "game/flow/table.hpp"
#include "hero/catalog.hpp"
#include "util/rng.hpp"
#include "util/types.hpp"

namespace tkw
{
    namespace game
    {
        /** @brief 建局入参：只含装配所需字段。 */
        struct BuildOptions
        {
            std::filesystem::path deck = "resources"; /**< 牌堆资源目录（`deck.json` + `cards/`）。 */
            int players = 4;                          /**< 玩家数（座位 `0..players-1`）。 */
            std::uint32_t seed = 42;                  /**< 随机种子（构造 `SeededRng`，不消费随机流）。 */
            GameMode mode = GameMode::Brawl;          /**< 对局模式（`Identity` 时分配角色）。 */
            std::map<std::string, std::string> heroes; /**< 座位 id → 武将 id（缺省空 = 全通用）。 */

            /** @brief 默认构造：`deck` 为 `resources`、4 名玩家、种子 42、乱斗模式。 */
            BuildOptions() = default;
            BuildOptions(const BuildOptions &) = default; /**< 拷贝构造：因自定义移动会抑制隐式拷贝，故显式保留。 */
            BuildOptions &operator=(const BuildOptions &) = default; /**< 拷贝赋值：语义同成员逐个拷贝。 @return `*this`。 */
            /**
             * @brief  移动构造：显式 `noexcept`。
             * @details `std::map` 的移动只窃取节点、实际不抛，但 MSVC 调试构建下
             *          未标记 `noexcept`；显式 `noexcept` 使本结构可放入 `Result`
             *          （其存储要求 `T` 移动构造为 `noexcept`）。
             * @param[in,out] other 源对象；`heroes` 被移动，其余基础字段按值搬运。
             */
            BuildOptions(BuildOptions &&other) noexcept
                : deck(std::move(other.deck)), players(other.players),
                  seed(other.seed), mode(other.mode),
                  heroes(std::move(other.heroes))
            {
            }
            /**
             * @brief  移动赋值。
             * @param[in,out] other 源对象；`heroes` 被移动，其余基础字段按值搬运。
             * @return `*this`。
             */
            BuildOptions &operator=(BuildOptions &&other) noexcept
            {
                deck = std::move(other.deck);
                players = other.players;
                seed = other.seed;
                mode = other.mode;
                heroes = std::move(other.heroes);
                return *this;
            }

            /**
             * @brief  常用四字段装配：`heroes` 留空。
             * @param[in] deck    牌堆资源目录。
             * @param[in] players 玩家数（座位 `0..players-1`）。
             * @param[in] seed    随机种子。
             * @param[in] mode    对局模式。
             */
            BuildOptions(std::filesystem::path deck, int players,
                         std::uint32_t seed, GameMode mode)
                : deck(std::move(deck)), players(players), seed(seed),
                  mode(mode)
            {
            }
        };

        /** @brief 建局失败信息：失败阶段 + 阶段上下文。 */
        struct BuildError
        {
            /** @brief 失败阶段。 */
            enum class Kind : std::uint8_t
            {
                LoadDeck,            /**< 牌堆/武将目录加载失败（`config` 携带 kind/detail）。 */
                CreatePlayer,        /**< 某座位玩家实体创建失败（`player_index` 指出座位）。 */
                IdentityPlayerCount, /**< 身份局人数无配比（`player_index` 指出人数）。 */
                UnknownHero,         /**< 指定武将不在目录中（`player_index` + `hero`）。 */
            };

            Kind kind = Kind::LoadDeck;  /**< 失败阶段。 */
            config::ConfigError config;  /**< `LoadDeck` 阶段的目录错误。 */
            int player_index = 0;        /**< `CreatePlayer`/`UnknownHero` 阶段的失败座位下标。 */
            std::string hero;            /**< `UnknownHero` 阶段请求的武将 id。 */
        };

        /**
         * @brief 建局结果别名。
         * @tparam T 成功时承载的值类型。
         */
        template <typename T>
        using BuildResult = Result<T, BuildError>;

        /**
         * @brief 建局装配（静态工具类）。
         * @details 纯装配过程：身份局人数校验、加载牌堆与武将目录、构造 `Game`、
         *          逐座创建玩家（身份局再洗牌分配角色）。不持有状态，入参由调用方
         *          传入。
         */
        class GameFactory
        {
        public:
            /** @brief 静态工具类，不可实例化。 */
            GameFactory() = delete;

            /**
             * @brief  性别占位：按座位奇偶交替（`P0` 男 / `P1` 女 / …）。
             * @param[in] seat 座位下标。
             * @return 偶数座位 `Male`，奇数座位 `Female`。
             * @note  当前无玩家数据源；武将自带性别时以武将值为准。
             */
            static entity::Gender gender_for_seat(int seat);

            /**
             * @brief  装配一局：加载牌堆与武将目录 → 建 `Game`（注入 `SeededRng`）→
             *         逐座创建玩家（身份局再分配角色）。
             * @details 装配顺序：身份局人数校验 → 加载牌堆目录 → 加载武将目录（可选）
             *          → 构造 `Game` → 逐座创建玩家 → 身份局洗牌分配角色。
             * @param[in] opt 建局入参；仅 `deck`/`players`/`seed`/`mode`/`heroes` 参与装配。
             * @return 建局结果。
             * @retval Ok  新一局已完整装配（玩家、目录、随机源就绪）。
             * @retval Err 建局失败，`BuildError::kind` 指出阶段：
             *             `LoadDeck` = 牌堆或武将目录加载失败（`config.kind/detail`）；
             *             `CreatePlayer` = `player_index` 座位实体创建失败；
             *             `IdentityPlayerCount` = 身份局人数无配比（`player_index` = 人数）；
             *             `UnknownHero` = `player_index` + `hero` 指定武将不在目录中。
             * @note  只构造 `SeededRng` 不消费随机流；武将赋值不消费随机流。仅
             *         `Identity` 模式用该随机源洗牌分配角色，`Brawl` 分支不消费随机流
             *         也不写角色。
             * @note  无 `heroes` 指定时逐座 hero 为空、性别与体力走现状；武将自带的
             *         性别与体力（`hp > 0`）覆盖座位占位，不影响其他玩家。
             * @see   BuildOptions, BuildError, start_session
             */
            static BuildResult<std::unique_ptr<Game>> build(const BuildOptions &opt);
        };

        inline entity::Gender GameFactory::gender_for_seat(int seat)
        {
            return seat % 2 == 0 ? entity::Gender::Male : entity::Gender::Female;
        }

    }
}

#endif  // INCLUDE_TKW_GAME_FACTORY_HPP
