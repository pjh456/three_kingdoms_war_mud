/**
 * @file   snapshot.cpp
 * @brief  TUI 视图快照实现：由 CLI 会话抽取四面板纯数据值。
 * @ingroup tkw_tui
 */
#include "tui/snapshot.hpp"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace tkw
{
    namespace tui
    {
        UiSnapshot make_snapshot(const tkw::cli::Session &s,
                                 const std::string &viewer)
        {
            UiSnapshot snap;
            snap.viewer = viewer;
            if (!s.active || !s.game)
                return snap;

            auto ctx = s.game->context();
            const tkw::game::ReadOnlyContext ro = ctx;

            snap.active = true;
            snap.over = tkw::game::SessionQuery::session_over(ctx);
            snap.at_cap =
                !snap.over && s.state.turns > tkw::game::rules_of(ctx).max_turns;
            snap.mode = tkw::game::mode_of(ctx);
            snap.current = s.state.current;
            snap.turns = s.state.turns;
            snap.alive = ctx.entities->size();
            snap.winner = tkw::game::SessionQuery::session_winner(ctx);
            snap.winner_label =
                snap.mode == tkw::game::GameMode::Brawl
                    ? tkw::cli::detail::winner_label(snap.winner)
                    : tkw::cli::detail::identity_result_label(
                          tkw::game::SessionQuery::session_camp(ctx), snap.winner);
            snap.ai = s.ai;
            snap.deck = s.deck;
            snap.humans = s.humans;

            snap.draw_size = ctx.cards->draw_size();
            snap.discard_size = ctx.cards->discard_size();

            for (const auto &id : ro.entities->ordered_ids())
            {
                const auto found = ro.entities->find(id);
                if (found.is_none())
                    continue;
                const auto *entity = found.unwrap();

                PlayerRow row;
                row.id = id;
                row.seat = entity->get_seat();
                row.hp = entity->get_hp();
                row.max_hp = entity->get_hp_bar().get_max();
                row.hand = visible_hand(ro, viewer, id);
                row.equip = public_zone(ro, ro.cards->equip(id));
                row.judge = public_zone(ro, ro.cards->judge(id));
                row.distance =
                    tkw::game::DistanceQuery::distance_between(ro, viewer, id);
                row.in_attack_range =
                    tkw::game::DistanceQuery::in_attack_range(ro, viewer, id);
                const tkw::game::Role role = tkw::game::role_of(ro.roles, id);
                row.role = role_visible(s.humans, snap.over, id, role)
                               ? role
                               : tkw::game::Role::None;
                row.chained = entity->get_chained();
                row.hero = tkw::hero::display_hero_name(
                    ro.heroes, entity->get_hero());
                snap.players.push_back(std::move(row));
            }

            return snap;
        }
    }  // namespace tui
}  // namespace tkw
