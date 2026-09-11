/**
 * @file factory.hpp
 * @brief 建局装配：加载牌堆目录 → 建 Game（注入随机源）→ 逐座创建玩家。
 * @note game 层建局入口，不依赖 CLI 类型；hand/AI/verbose 等会话参数不参与
 *       装配，由调用方在开局准备（start_session）与决策源构造处消费。
 */

#ifndef INCLUDE_TKW_GAME_FACTORY_HPP
#define INCLUDE_TKW_GAME_FACTORY_HPP

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "card/catalog.hpp"
#include "config/error.hpp"
#include "config/resource.hpp"
#include "entity/base.hpp"
#include "entity/hp.hpp"
#include "game/flow/table.hpp"
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
        };

        /** @brief 建局失败信息：失败阶段 + 阶段上下文。 */
        struct BuildError
        {
            /** 失败阶段。 */
            enum class Kind : std::uint8_t
            {
                LoadDeck,     /**< 牌堆目录加载失败（config 携带 kind/detail） */
                CreatePlayer, /**< 某座位玩家实体创建失败（player_index 指出座位） */
            };

            Kind kind = Kind::LoadDeck;  /**< 失败阶段 */
            config::ConfigError config;  /**< LoadDeck 阶段的目录错误 */
            int player_index = 0;        /**< CreatePlayer 阶段的失败座位下标 */
        };

        template <typename T>
        using BuildResult = Result<T, BuildError>;

        /** 性别占位：无玩家数据源，按座位奇偶交替（P0 男 / P1 女 / …）。 */
        inline entity::Gender gender_for_seat(int seat)
        {
            return seat % 2 == 0 ? entity::Gender::Male : entity::Gender::Female;
        }

        /**
         * @brief 装配一局：加载牌堆目录 → 建 Game（注入 SeededRng）→ 逐座创建玩家。
         * @param opt deck/players/seed；其余会话参数不参与装配。
         * @return Ok 持有新一局；Err LoadDeck 为目录加载失败（config.kind/detail），
         *         CreatePlayer 为该座位实体创建失败（player_index）。
         * @note 只构造 SeededRng 不消费随机流，不发卡牌事件，故装配本身行为确定，
         *       与后续发牌（start_session）解耦。
         */
        inline BuildResult<std::unique_ptr<Game>> build_game(const BuildOptions &opt)
        {
            config::ResourceStore store(opt.deck);
            auto catalog = card::CardDefCatalog::load(store, "deck");
            if (catalog.is_err())
                return BuildResult<std::unique_ptr<Game>>::Err(
                    BuildError{BuildError::Kind::LoadDeck, catalog.unwrap_err(), 0});

            auto game = std::make_unique<Game>(
                std::move(catalog).unwrap(),
                std::make_unique<SeededRng>(opt.seed));

            for (int i = 0; i < opt.players; ++i)
            {
                auto r = game->add_player(
                    "P" + std::to_string(i), i, entity::Hp::make(game->rules.base_hp),
                    gender_for_seat(i));
                if (r.is_err())
                    return BuildResult<std::unique_ptr<Game>>::Err(
                        BuildError{BuildError::Kind::CreatePlayer, {}, i});
            }
            return BuildResult<std::unique_ptr<Game>>::Ok(std::move(game));
        }
    }
}

#endif  // INCLUDE_TKW_GAME_FACTORY_HPP
