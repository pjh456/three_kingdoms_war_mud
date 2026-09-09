/**
 * @file manager.hpp
 * @brief 对局作用域的卡牌容器：摸牌堆/弃牌堆 + 按 entity id 键控的
 *        手牌区/装备区/判定区（三者共用一个 CardZone 抽象，区以三元素
 *        数组存放）。
 * @note 哑状态持有者（对齐 entity/manager.hpp 的定位）：
 *       - 不校验规则：装备槽位占用、手牌上限、摸空补牌等归 gameplay；
 *       - 不发布事件：摸/弃/打出由 gameplay 观察返回值并自行发布；
 *       - 不持有 CardDefCatalog：需要 type/effect/equip 时由 gameplay 经
 *         catalog 解析，保持本模块无目录依赖。
 */

#ifndef INCLUDE_TKW_CARD_MANAGER_HPP
#define INCLUDE_TKW_CARD_MANAGER_HPP

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "card/card.hpp"
#include "card/catalog.hpp"
#include "card/def.hpp"
#include "util/rng.hpp"
#include "util/types.hpp"

namespace tkw
{
    namespace card
    {
        /** @brief 一个按 instance_id 管理的牌区（手牌/装备/判定共用）。 */
        class CardZone
        {
        public:
            void add(Card card) { cards_.push_back(std::move(card)); }

            /** @brief 按 instance_id 移除并返回；不存在时 None。 */
            Option<Card> remove(const std::string &instance_id)
            {
                const auto it = std::find_if(
                    cards_.begin(), cards_.end(),
                    [&](const Card &c) { return c.instance_id == instance_id; });
                if (it == cards_.end())
                    return Option<Card>::None();
                Card c = std::move(*it);
                cards_.erase(it);
                return Option<Card>::Some(std::move(c));
            }

            std::size_t size() const noexcept { return cards_.size(); }
            bool empty() const noexcept { return cards_.empty(); }
            const std::vector<Card> &view() const noexcept { return cards_; }

            /** @brief 取走全部牌（死亡清场）。 */
            std::vector<Card> drain() { return std::move(cards_); }

        private:
            std::vector<Card> cards_;
        };

        /**
         * @brief 卡牌容器完整快照（保序；区域按 entity id 排序保证确定性）。
         */
        struct CardManagerSnapshot
        {
            std::uint64_t instance_seq = 0;
            std::vector<Card> draw;    /**< 堆底→堆顶 */
            std::vector<Card> discard; /**< 堆底→堆顶 */
            std::vector<std::pair<std::string, std::vector<Card>>> hand;
            std::vector<std::pair<std::string, std::vector<Card>>> equip;
            std::vector<std::pair<std::string, std::vector<Card>>> judge;
        };

        /**
         * @class CardManager
         * @brief 卡牌容器 + 按 entity id 的区（hand/equip/judge）。
         * @note 实体引用一律用 id 字符串（跨域约定），不依赖 entity 模块。
         */
        class CardManager
        {
        public:
            CardManager() = default;
            CardManager(const CardManager &) = delete;
            CardManager &operator=(const CardManager &) = delete;
            CardManager(CardManager &&) noexcept = default;
            CardManager &operator=(CardManager &&) noexcept = default;

            /**
             * @brief 按目录构建摸牌堆：每份副本生成一张实体牌并分配唯一 instance_id。
             * @note 卡牌顺序 = 目录迭代序（deck.json 引用顺序），同 seed 下确定。
             * @note 幂等：先清空摸牌堆，重复调用不会叠加重复牌。
             */
            void build_deck(const CardDefCatalog &catalog)
            {
                draw_pile = CardStack{};
                for (const auto &def : catalog)
                {
                    for (const auto &copy : def.copies)
                        draw_pile.push(make_card(def.id, copy));
                }
            }

            /** @brief 完整快照（牌堆保序；区域按 entity id 排序）。 */
            CardManagerSnapshot snapshot() const
            {
                CardManagerSnapshot s;
                s.instance_seq = instance_seq;
                s.draw = draw_pile.view();
                s.discard = discard_pile.view();
                s.hand = zone_snapshot(zones_of(Zone::Hand));
                s.equip = zone_snapshot(zones_of(Zone::Equip));
                s.judge = zone_snapshot(zones_of(Zone::Judge));
                return s;
            }

            /** @brief 从快照恢复：清空后按序重建（含 instance_seq）。 */
            void restore(const CardManagerSnapshot &s)
            {
                clear();
                instance_seq = s.instance_seq;
                for (const auto &c : s.draw)
                    draw_pile.push(c);
                for (const auto &c : s.discard)
                    discard_pile.push(c);
                restore_zone(zones_of(Zone::Hand), s.hand);
                restore_zone(zones_of(Zone::Equip), s.equip);
                restore_zone(zones_of(Zone::Judge), s.judge);
            }

            /** @brief 清空全部牌与实例序号。 */
            void clear()
            {
                instance_seq = 0;
                draw_pile = CardStack{};
                discard_pile = CardStack{};
                for (auto &zones : entity_zones)
                    zones.clear();
            }

            // ── 摸牌堆 / 弃牌堆 ──────────────────────────────────────────

            /** @brief 摸顶牌；摸空时 None（不自动洗回弃牌堆）。 */
            Option<Card> draw() { return draw_pile.pop(); }

            /** @brief 弃牌（置弃牌堆顶）。 */
            void discard(Card card) { discard_pile.push(std::move(card)); }

            /** @brief 从弃牌堆取回一张牌（结算回滚用）；不存在时 None。 */
            Option<Card> remove_from_discard(const std::string &instance_id)
            {
                return discard_pile.remove(instance_id);
            }

            /** @brief 置摸牌堆顶（种牌堆/结算后回置等）。 */
            void add_to_draw(Card card) { draw_pile.push(std::move(card)); }

            /**
             * @brief 弃牌堆整体洗回摸牌堆（判定/摸牌时牌堆空的补牌）。
             * @note 弃牌堆为空时无操作；转移后原地洗牌。
             */
            void refill_draw(Rng &rng)
            {
                while (true)
                {
                    auto c = discard_pile.pop();
                    if (c.is_none())
                        break;
                    draw_pile.push(std::move(c).unwrap());
                }
                if (draw_pile.size() > 1)
                    draw_pile.shuffle(rng);
            }

            std::size_t draw_size() const noexcept { return draw_pile.size(); }
            std::size_t discard_size() const noexcept { return discard_pile.size(); }

            void shuffle_draw(Rng &rng) { draw_pile.shuffle(rng); }
            Option<const Card *> draw_top() const { return draw_pile.top(); }

            // ── 手牌区 ──────────────────────────────────────────────────

            void add_to_hand(const std::string &entity_id, Card card)
            {
                zones_of(Zone::Hand)[entity_id].add(std::move(card));
            }

            Option<Card> remove_from_hand(
                const std::string &entity_id, const std::string &instance_id)
            {
                return remove_from_zone(Zone::Hand, entity_id, instance_id);
            }

            std::size_t hand_size(const std::string &entity_id) const
            {
                return zone_size(Zone::Hand, entity_id);
            }

            /** @brief 手牌列表（不存在实体时为空列表）。 */
            const std::vector<Card> &hand(const std::string &entity_id) const
            {
                return zone_view(Zone::Hand, entity_id);
            }

            // ── 装备区 ──────────────────────────────────────────────────

            void add_to_equip(const std::string &entity_id, Card card)
            {
                zones_of(Zone::Equip)[entity_id].add(std::move(card));
            }

            Option<Card> remove_from_equip(
                const std::string &entity_id, const std::string &instance_id)
            {
                return remove_from_zone(Zone::Equip, entity_id, instance_id);
            }

            std::size_t equip_size(const std::string &entity_id) const
            {
                return zone_size(Zone::Equip, entity_id);
            }

            const std::vector<Card> &equip(const std::string &entity_id) const
            {
                return zone_view(Zone::Equip, entity_id);
            }

            // ── 判定区 ──────────────────────────────────────────────────

            void add_to_judge(const std::string &entity_id, Card card)
            {
                zones_of(Zone::Judge)[entity_id].add(std::move(card));
            }

            Option<Card> remove_from_judge(
                const std::string &entity_id, const std::string &instance_id)
            {
                return remove_from_zone(Zone::Judge, entity_id, instance_id);
            }

            std::size_t judge_size(const std::string &entity_id) const
            {
                return zone_size(Zone::Judge, entity_id);
            }

            const std::vector<Card> &judge(const std::string &entity_id) const
            {
                return zone_view(Zone::Judge, entity_id);
            }

            // ── 跨区操作 ────────────────────────────────────────────────

            /**
             * @brief 从任一实体区域移除（hand → equip → judge 顺序）。
             * @param from 非空时写入来源区域。
             */
            Option<Card> remove_from_any(
                const std::string &entity_id, const std::string &instance_id,
                Zone *from = nullptr)
            {
                for (const Zone z : kSlotZones)
                {
                    if (auto c = remove_from_zone(z, entity_id, instance_id);
                        c.is_some())
                    {
                        if (from)
                            *from = z;
                        return c;
                    }
                }
                return Option<Card>::None();
            }

            /**
             * @brief 该牌是否在实体的任一区域（hand/equip/judge）。
             * @note 结算前校验用：避免决策源返回不存在的牌时才在结算中途失败。
             */
            bool has_card(
                const std::string &entity_id, const std::string &instance_id) const
            {
                for (const Zone z : kSlotZones)
                {
                    const auto *zone = find_zone(z, entity_id);
                    if (zone == nullptr)
                        continue;
                    for (const auto &c : zone->view())
                        if (c.instance_id == instance_id)
                            return true;
                }
                return false;
            }

            /**
             * @brief 死亡清场：手牌/装备/判定区全部置入弃牌堆。
             * @return 被弃置的牌（供调用方发布弃置事件）。
             */
            std::vector<Card> discard_all(const std::string &entity_id)
            {
                std::vector<Card> out;
                for (const Zone z : kSlotZones)
                {
                    ZoneMap &zones = zones_of(z);
                    auto it = zones.find(entity_id);
                    if (it == zones.end())
                        continue;
                    auto cards = it->second.drain();
                    for (auto &c : cards)
                    {
                        out.push_back(c);
                        discard_pile.push(std::move(c));
                    }
                    zones.erase(it);
                }
                return out;
            }

        private:
            using ZoneMap = std::unordered_map<std::string, CardZone>;

            std::uint64_t instance_seq = 0;
            CardStack draw_pile;
            CardStack discard_pile;
            std::array<ZoneMap, 3> entity_zones; /**< 槽位顺序见 kSlotZones */

            /**
             * @brief 实体区 → 槽位下标（Hand=0 / Equip=1 / Judge=2）。
             * @note 不变量：仅 Hand/Equip/Judge 三区合法且必须与 kSlotZones
             *       同序；其余 Zone 值（Draw/Discard/Limbo）不进入实体区。
             */
            static constexpr int zone_slot(Zone zone)
            {
                switch (zone)
                {
                    case Zone::Hand:
                        return 0;
                    case Zone::Equip:
                        return 1;
                    case Zone::Judge:
                        return 2;
                    default:
                        return -1;
                }
            }

            /** @brief 槽位 → 区域表：跨区操作统一按此序（Hand → Equip → Judge）。 */
            static constexpr std::array<Zone, 3> kSlotZones = {
                Zone::Hand, Zone::Equip, Zone::Judge};

            ZoneMap &zones_of(Zone zone) { return entity_zones[zone_slot(zone)]; }

            const ZoneMap &zones_of(Zone zone) const
            {
                return entity_zones[zone_slot(zone)];
            }

            static std::vector<std::pair<std::string, std::vector<Card>>> zone_snapshot(
                const ZoneMap &zones)
            {
                std::vector<std::pair<std::string, std::vector<Card>>> out;
                out.reserve(zones.size());
                for (const auto &[id, zone] : zones)
                {
                    if (zone.empty())  // 空区域不入快照（规范化，便于往返稳定）
                        continue;
                    out.emplace_back(id, zone.view());
                }
                std::sort(out.begin(), out.end(),
                          [](const auto &a, const auto &b)
                          { return a.first < b.first; });
                return out;
            }

            static void restore_zone(
                ZoneMap &zones,
                const std::vector<std::pair<std::string, std::vector<Card>>> &in)
            {
                for (const auto &[id, cards] : in)
                    for (const auto &c : cards)
                        zones[id].add(c);
            }

            static const std::vector<Card> &empty_list()
            {
                static const std::vector<Card> empty;
                return empty;
            }

            CardZone *find_zone(Zone zone, const std::string &entity_id)
            {
                ZoneMap &zones = zones_of(zone);
                auto it = zones.find(entity_id);
                return it == zones.end() ? nullptr : &it->second;
            }

            const CardZone *find_zone(
                Zone zone, const std::string &entity_id) const
            {
                const ZoneMap &zones = zones_of(zone);
                auto it = zones.find(entity_id);
                return it == zones.end() ? nullptr : &it->second;
            }

            Option<Card> remove_from_zone(
                Zone zone, const std::string &entity_id,
                const std::string &instance_id)
            {
                auto *z = find_zone(zone, entity_id);
                return z ? z->remove(instance_id) : Option<Card>::None();
            }

            std::size_t zone_size(Zone zone, const std::string &entity_id) const
            {
                const auto *z = find_zone(zone, entity_id);
                return z ? z->size() : 0;
            }

            const std::vector<Card> &zone_view(
                Zone zone, const std::string &entity_id) const
            {
                const auto *z = find_zone(zone, entity_id);
                return z ? z->view() : empty_list();
            }

            Card make_card(const std::string &def_id, const CardCopy &copy)
            {
                return Card{
                    def_id + "#" + std::to_string(instance_seq++), def_id, copy.suit,
                    copy.number};
            }
        };
    }
}

#endif  // INCLUDE_TKW_CARD_MANAGER_HPP
