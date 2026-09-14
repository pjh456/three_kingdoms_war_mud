/**
 * @file factory.hpp
 * @brief 建局装配：加载牌堆目录 → 建 Game（注入随机源）→ 逐座创建玩家。
 * @note game 层建局入口，不依赖 CLI 类型；hand/AI/verbose 等会话参数不参与
 *       装配，由调用方在开局准备（start_session）与决策源构造处消费。
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
            std::filesystem::path deck = "resources"; /**< 牌堆资源目录（deck.json + cards/） */
            int players = 4;                          /**< 玩家数（座位 0..players-1） */
            std::uint32_t seed = 42;                  /**< 随机种子（构造 SeededRng，不消费流） */
            GameMode mode = GameMode::Brawl;          /**< 对局模式（identity 时分配角色） */
            std::map<std::string, std::string> heroes; /**< 座位 id → 武将 id（缺省空 = 全通用） */

            /**
             * @brief 显式声明移动/拷贝：std::map 的移动只窃取节点、实际不抛，但
             *        MSVC 调试构建下未标记 noexcept；显式 noexcept 移动使本结构可
             *        放入 Result（其存储要求 T 移动构造为 noexcept）。自定义移动会
             *        抑制隐式拷贝，故拷贝一并保留。
             */
            BuildOptions() = default;
            BuildOptions(const BuildOptions &) = default;
            BuildOptions &operator=(const BuildOptions &) = default;
            BuildOptions(BuildOptions &&other) noexcept
                : deck(std::move(other.deck)), players(other.players),
                  seed(other.seed), mode(other.mode),
                  heroes(std::move(other.heroes))
            {
            }
            BuildOptions &operator=(BuildOptions &&other) noexcept
            {
                deck = std::move(other.deck);
                players = other.players;
                seed = other.seed;
                mode = other.mode;
                heroes = std::move(other.heroes);
                return *this;
            }

            /** @brief 常用四字段装配：heroes 留空，等价于原聚合初始化。 */
            BuildOptions(std::filesystem::path deck_, int players_,
                         std::uint32_t seed_, GameMode mode_)
                : deck(std::move(deck_)), players(players_), seed(seed_),
                  mode(mode_)
            {
            }
        };

        /** @brief 建局失败信息：失败阶段 + 阶段上下文。 */
        struct BuildError
        {
            /** 失败阶段。 */
            enum class Kind : std::uint8_t
            {
                LoadDeck,            /**< 牌堆/武将目录加载失败（config 携带 kind/detail） */
                CreatePlayer,        /**< 某座位玩家实体创建失败（player_index 指出座位） */
                IdentityPlayerCount, /**< 身份局人数无配比（player_index 指出人数） */
                UnknownHero,         /**< 某座位指定了目录中不存在的武将（player_index + hero） */
            };

            Kind kind = Kind::LoadDeck;  /**< 失败阶段 */
            config::ConfigError config;  /**< LoadDeck 阶段的目录错误 */
            int player_index = 0;        /**< CreatePlayer/UnknownHero 阶段的失败座位下标 */
            std::string hero;            /**< UnknownHero 阶段请求的武将 id */
        };

        template <typename T>
        using BuildResult = Result<T, BuildError>;

        /** 性别占位：无玩家数据源，按座位奇偶交替（P0 男 / P1 女 / …）。 */
        inline entity::Gender gender_for_seat(int seat)
        {
            return seat % 2 == 0 ? entity::Gender::Male : entity::Gender::Female;
        }

        /**
         * @brief 装配一局：加载牌堆与武将目录 → 建 Game（注入 SeededRng）→ 逐座
         *        创建玩家（身份局再分配角色）。
         * @param opt deck/players/seed/mode/heroes；其余会话参数不参与装配。
         * @return Ok 持有新一局；Err LoadDeck 为目录加载失败（config.kind/detail，
         *         牌堆与武将目录共用本阶段），CreatePlayer 为该座位实体创建失败
         *         （player_index），IdentityPlayerCount 为身份局人数无配比
         *         （player_index = 人数），UnknownHero 为该座位武将不在目录中
         *         （player_index + hero）。
         * @note 只构造 SeededRng 不消费随机流；武将赋值不消费随机流。仅 identity
         *       模式用该随机源洗牌分配角色，brawl 分支不消费随机流也不写角色。
         * @note 无 heroes 指定时逐座 hero 为空、性别与体力走现状；武将自带的
         *       性别与体力（hp > 0）覆盖座位占位，不影响其他玩家。
         */
        inline BuildResult<std::unique_ptr<Game>> build_game(const BuildOptions &opt)
        {
            if (opt.mode == GameMode::Identity && roles_for_count(opt.players).is_none())
                return BuildResult<std::unique_ptr<Game>>::Err(
                    BuildError{BuildError::Kind::IdentityPlayerCount, {}, opt.players});

            config::ResourceStore store(opt.deck);
            auto catalog = card::CardDefCatalog::load(store, "deck");
            if (catalog.is_err())
                return BuildResult<std::unique_ptr<Game>>::Err(
                    BuildError{BuildError::Kind::LoadDeck, catalog.unwrap_err(), 0});

            // 武将目录可选：缺 heroes.json 回落空目录，坏数据（解析/枚举）仍硬失败
            auto hero_catalog = hero::HeroCatalog::load_optional(store, "heroes");
            if (hero_catalog.is_err())
                return BuildResult<std::unique_ptr<Game>>::Err(
                    BuildError{BuildError::Kind::LoadDeck, hero_catalog.unwrap_err(), 0});

            auto game = std::make_unique<Game>(
                std::move(catalog).unwrap(),
                std::make_unique<SeededRng>(opt.seed));
            game->hero_catalog = std::move(hero_catalog).unwrap();

            for (int i = 0; i < opt.players; ++i)
            {
                const std::string seat = "P" + std::to_string(i);
                entity::Gender gender = gender_for_seat(i);
                int max_hp = game->rules.base_hp;
                std::string hero_id;

                const auto pick = opt.heroes.find(seat);
                if (pick != opt.heroes.end())
                {
                    const auto def = game->hero_catalog.find(pick->second);
                    if (def.is_none())
                        return BuildResult<std::unique_ptr<Game>>::Err(
                            BuildError{BuildError::Kind::UnknownHero, {}, i,
                                       pick->second});
                    const hero::HeroDef &h = *def.unwrap();
                    if (h.gender.is_some())
                        gender = h.gender.unwrap();
                    if (h.hp > 0)
                        max_hp = h.hp;
                    hero_id = pick->second;
                }

                auto r = game->add_player(
                    seat, i, entity::Hp::make(max_hp), gender, hero_id);
                if (r.is_err())
                    return BuildResult<std::unique_ptr<Game>>::Err(
                        BuildError{BuildError::Kind::CreatePlayer, {}, i});
            }

            if (opt.mode == GameMode::Identity)
            {
                const RoleCounts counts = roles_for_count(opt.players).unwrap();
                game->mode = GameMode::Identity;
                game->roles["P0"] = Role::Lord;

                std::vector<Role> pool;
                pool.reserve(static_cast<std::size_t>(opt.players - 1));
                for (int i = 0; i < counts.loyalist; ++i)
                    pool.push_back(Role::Loyalist);
                for (int i = 0; i < counts.rebel; ++i)
                    pool.push_back(Role::Rebel);
                for (int i = 0; i < counts.traitor; ++i)
                    pool.push_back(Role::Traitor);

                // Fisher-Yates：座位 1..n-1 逐一从剩余池取角色，消耗随机流
                for (int i = static_cast<int>(pool.size()) - 1; i > 0; --i)
                {
                    const int j = static_cast<int>(uniform_below(
                        *game->rng, static_cast<std::uint32_t>(i + 1)));
                    std::swap(pool[static_cast<std::size_t>(i)],
                              pool[static_cast<std::size_t>(j)]);
                }

                for (int seat = 1; seat < opt.players; ++seat)
                    game->roles["P" + std::to_string(seat)] =
                        pool[static_cast<std::size_t>(seat - 1)];
            }

            return BuildResult<std::unique_ptr<Game>>::Ok(std::move(game));
        }
    }
}

#endif  // INCLUDE_TKW_GAME_FACTORY_HPP
