/**
 * @file test_game.hpp
 * @brief 共享测试夹具：TestGame（Game + 发牌/装备辅助）与 TestDecider（脚本化决策源）。
 */

#ifndef INCLUDE_TKW_TESTS_GAME_TEST_GAME_HPP
#define INCLUDE_TKW_TESTS_GAME_TEST_GAME_HPP

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "card/card.hpp"
#include "card/catalog.hpp"
#include "card/def.hpp"
#include "config/resource.hpp"
#include "entity/hp.hpp"
#include "game/core/decision.hpp"
#include "game/flow/table.hpp"
#include "game/query/equip.hpp"
#include "util/rng.hpp"

namespace tkw
{
    namespace test
    {
        using namespace tkw::game;
        using tkw::Option;
        using tkw::card::Ability;
        using tkw::card::Card;
        using tkw::card::CardDefCatalog;
        using tkw::card::ResponseKind;
        using tkw::entity::Entity;
        using tkw::entity::Gender;
        using tkw::entity::Hp;

        /** 测试对局：Game 应用层 + 测试辅助（发牌/装备）。 */
        struct TestGame : Game
        {
            GameContext ctx;

            explicit TestGame(const char *deck_name, std::uint32_t seed = 1) :
                Game(load_catalog(deck_name), std::make_unique<tkw::SeededRng>(seed)),
                ctx(context())
            {
            }

            Entity *add_player(
                const std::string &id, int seat, int hp,
                Gender gender = Gender::Male)
            {
                auto r = entities.create(id, seat, Hp::make(hp), gender);
                REQUIRE(r.is_ok());
                return r.unwrap();
            }

            void give(const std::string &id, const std::string &def_id, const char *inst)
            {
                const auto def = catalog.find(def_id);
                REQUIRE(def.is_some());
                const auto &copy = def.unwrap()->copies[0];
                cards.add_to_hand(id, Card{inst, def_id, copy.suit, copy.number});
            }

            void equip(const std::string &id, const std::string &def_id, const char *inst)
            {
                const auto def = catalog.find(def_id);
                REQUIRE(def.is_some());
                const auto &copy = def.unwrap()->copies[0];
                cards.add_to_equip(id, Card{inst, def_id, copy.suit, copy.number});
            }

            static CardDefCatalog load_catalog(const char *name)
            {
                tkw::config::ResourceStore store(TKW_TEST_RESOURCE_DIR);
                auto r = CardDefCatalog::load(store, name);
                REQUIRE(r.is_ok());
                return std::move(r).unwrap();
            }
        };

        /** 确定性决策源：respond=是否总是打出响应牌（杀响应无真杀且装备
         *  丈八蛇矛时改为两张手牌当杀）；save=是否有桃就救；counter=有无懈就出；
         *  triggers=会发动的装备效果；plays=出牌脚本（依次执行，耗尽即结束
         *  出牌）；弃牌=手牌前 count 张。 */
        struct TestDecider : DecisionSource
        {
            bool respond = false;
            bool save = false;
            bool counter = false;
            std::vector<std::vector<std::string>>
                counter_windows;  /**< 各无懈窗口携带的目标集合（按询问顺序） */
            bool bogus_pick = false;  /**< 选牌返回一张不存在的牌（校验测试用） */
            bool decline_discards = false; /**< 弃牌选择返回空（雌雄二选一的放弃分支） */
            std::string response_id;  /**< 非空时响应窗口固定打出该牌 */
            std::string response_second_id; /**< 非空时与 response_id 成对（两张当杀） */
            std::vector<Ability> triggers;
            std::vector<Ability> trigger_calls; /**< 实际被询问的发动能力（按顺序） */
            std::vector<PlayAction> plays;
            std::size_t play_cursor = 0;

            Option<PlayAction> play_response(
                const ReadOnlyContext &ctx, const std::string &entity,
                ResponseKind kind) override
            {
                if (!response_id.empty())
                    return Option<PlayAction>::Some(
                        PlayAction{response_id, {}, response_second_id});
                if (!respond)
                    return Option<PlayAction>::None();
                for (const auto &c : ctx.cards->hand(entity))
                {
                    const auto def = ctx.catalog->find(c.def_id);
                    if (def.is_none() || def.unwrap()->effect.is_none())
                        continue;
                    const auto ek = def.unwrap()->effect.unwrap().kind;
                    if ((kind == ResponseKind::Sha &&
                         ek == tkw::card::CardEffectKind::Damage) ||
                        (kind == ResponseKind::Jink &&
                         ek == tkw::card::CardEffectKind::Jink))
                        return Option<PlayAction>::Some(PlayAction{c.instance_id, {}});
                }
                // 无真响应牌：丈八蛇矛两张手牌当杀（确定性首 pair，与主动侧一致）
                if (kind == ResponseKind::Sha &&
                    has_ability(ctx, entity, Ability::TwoCardsAsSha))
                {
                    const auto &hand = ctx.cards->hand(entity);
                    if (hand.size() >= 2)
                        return Option<PlayAction>::Some(
                            PlayAction{hand.front().instance_id, {},
                                      hand[1].instance_id});
                }
                return Option<PlayAction>::None();
            }

            Option<std::string> play_peach(
                const ReadOnlyContext &ctx, const std::string &saver,
                const std::string &) override
            {
                if (!save)
                    return Option<std::string>::None();
                for (const auto &c : ctx.cards->hand(saver))
                {
                    const auto def = ctx.catalog->find(c.def_id);
                    if (def.is_some() && def.unwrap()->rescue)
                        return Option<std::string>::Some(c.instance_id);
                }
                return Option<std::string>::None();
            }

            Option<std::string> play_counter(
                const ReadOnlyContext &ctx, const std::string &player,
                const std::string &,
                const std::vector<std::string> &trick_targets) override
            {
                counter_windows.push_back(trick_targets);
                if (!counter)
                    return Option<std::string>::None();
                for (const auto &c : ctx.cards->hand(player))
                {
                    const auto def = ctx.catalog->find(c.def_id);
                    if (def.is_some() && def.unwrap()->counter)
                        return Option<std::string>::Some(c.instance_id);
                }
                return Option<std::string>::None();
            }

            bool trigger_effect(
                const ReadOnlyContext &, const std::string &, Ability ability) override
            {
                trigger_calls.push_back(ability);
                return std::find(triggers.begin(), triggers.end(), ability) !=
                       triggers.end();
            }

            Option<card::Card> pick_card_from_target(
                const ReadOnlyContext &ctx, const std::string &,
                const std::string &target) override
            {
                if (bogus_pick)
                    return Option<card::Card>::Some(
                        Card{"ghost#0", "sha", tkw::card::Suit::Spade, 7});
                const auto &hand = ctx.cards->hand(target);
                if (hand.empty())
                    return Option<card::Card>::None();
                return Option<card::Card>::Some(hand.front());
            }

            Option<card::Card> pick_from_revealed(
                const ReadOnlyContext &, const std::string &,
                const std::vector<card::Card> &options) override
            {
                if (options.empty())
                    return Option<card::Card>::None();
                return Option<card::Card>::Some(options.front());
            }

            Option<PlayAction> choose_play(
                const ReadOnlyContext &, const TurnContext &) override
            {
                if (play_cursor >= plays.size())
                    return Option<PlayAction>::None();
                return Option<PlayAction>::Some(plays[play_cursor++]);
            }

            std::vector<std::string> choose_discards(
                const ReadOnlyContext &ctx, const std::string &player, int count,
                DiscardReason) override
            {
                if (decline_discards)
                    return {};
                const auto &hand = ctx.cards->hand(player);
                std::vector<std::string> out;
                for (int i = 0; i < count && i < static_cast<int>(hand.size()); ++i)
                    out.push_back(hand[i].instance_id);
                return out;
            }
        };
    }
}

#endif  // INCLUDE_TKW_TESTS_GAME_TEST_GAME_HPP
