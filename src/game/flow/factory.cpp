/**
 * @file   factory.cpp
 * @brief  建局装配（加载目录/建 Game/分配角色）的定义。
 * @ingroup tkw_game_flow
 */

#include "game/flow/factory.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace tkw
{
    namespace game
    {
        BuildResult<std::unique_ptr<Game>> GameFactory::build(
            const BuildOptions &opt)
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
                entity::Gender gender = GameFactory::gender_for_seat(i);
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
