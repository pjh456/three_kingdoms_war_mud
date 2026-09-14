/**
 * @file   catalog.cpp
 * @brief  卡牌 JSON 解析与目录加载的定义。
 * @ingroup tkw_card
 */

#include "card/catalog.hpp"

#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tkw
{
    namespace card
    {
        namespace detail
        {
            bool is_valid_subtype(std::string_view s)
            {
                return s.empty() || s == "attack" || s == "dodge" || s == "heal" ||
                       s == "instant" || s == "delayed" || s == "weapon" ||
                       s == "armor" || s == "horse";
            }

            cfg::ConfigResult<Option<const json::Json *>> opt_object(
                const json::Json &obj, std::string_view key, std::string_view path)
            {
                const auto *o = obj.try_as_object();
                if (!o)
                    return cfg::fail<Option<const json::Json *>>(
                        cfg::ConfigErrorKind::TypeMismatch,
                        cfg::container_path(path));
                if (!o->contains(key))
                    return cfg::ConfigResult<Option<const json::Json *>>::Ok(
                        Option<const json::Json *>::None());
                const json::Json &v = (*o)[key];
                if (!v.try_as_object())
                    return cfg::fail<Option<const json::Json *>>(
                        cfg::ConfigErrorKind::TypeMismatch, cfg::field_path(path, key));
                return cfg::ConfigResult<Option<const json::Json *>>::Ok(
                    Option<const json::Json *>::Some(&v));
            }

            cfg::ConfigResult<CardCopy> parse_card_copy(
                const json::Json &item, std::string_view ip)
            {
                // 资源 JSON schema 契约：`cards/*.json` 的 `"suit"` 字段值域由
                // 下方映射表定义，非法值加载失败。与存档文本
                // （`save::suit_name`/`suit_from`）取值相同，但分属两个独立稳定
                // 契约：此处是可编辑的资源数据格式，存档文本冻结在
                // `save/format.hpp`；不得合并。改值域须同步全部
                // `resources/<deck>/cards/*.json` 与 `save/format.{hpp,cpp}`。
                auto suit = require_enum<Suit>(
                    item, "suit", ip,
                    {{"spade", Suit::Spade}, {"club", Suit::Club},
                     {"heart", Suit::Heart}, {"diamond", Suit::Diamond}});
                if (suit.is_err())
                    return cfg::ConfigResult<CardCopy>::Err(suit.unwrap_err());

                auto num = cfg::require_int(item, "number", ip);
                if (num.is_err())
                    return cfg::ConfigResult<CardCopy>::Err(num.unwrap_err());
                const auto n = num.unwrap();
                if (n < 1 || n > 13)
                    return cfg::fail<CardCopy>(
                        cfg::ConfigErrorKind::InvalidValue, cfg::field_path(ip, "number"));

                return cfg::ConfigResult<CardCopy>::Ok(
                    CardCopy{suit.unwrap(), static_cast<int>(n)});
            }

            cfg::ConfigResult<CardEffect> parse_card_effect(
                const json::Json &obj, std::string_view path)
            {
                CardEffect eff;
                auto kind = require_enum<CardEffectKind>(
                    obj, "kind", path, effect_kind_table);
                if (kind.is_err())
                    return cfg::ConfigResult<CardEffect>::Err(kind.unwrap_err());
                eff.kind = kind.unwrap();

                auto amount = cfg::opt_int_range(obj, "amount", 0, path);
                if (amount.is_err())
                    return cfg::ConfigResult<CardEffect>::Err(amount.unwrap_err());
                eff.amount = amount.unwrap();

                auto count = cfg::opt_int_range(obj, "count", 0, path);
                if (count.is_err())
                    return cfg::ConfigResult<CardEffect>::Err(count.unwrap_err());
                eff.count = count.unwrap();

                auto scope = opt_enum<Scope>(obj, "scope", path, scope_table);
                if (scope.is_err())
                    return cfg::ConfigResult<CardEffect>::Err(scope.unwrap_err());
                eff.scope = scope.unwrap();

                auto resp = opt_enum<ResponseKind>(
                    obj, "response", path,
                    {{"sha", ResponseKind::Sha}, {"jink", ResponseKind::Jink}});
                if (resp.is_err())
                    return cfg::ConfigResult<CardEffect>::Err(resp.unwrap_err());
                eff.response = resp.unwrap();

                auto range = cfg::opt_int_range(obj, "range", 0, path);
                if (range.is_err())
                    return cfg::ConfigResult<CardEffect>::Err(range.unwrap_err());
                eff.range = range.unwrap();

                auto dtype =
                    opt_enum<DamageType>(obj, "damage_type", path, damage_type_table);
                if (dtype.is_err())
                    return cfg::ConfigResult<CardEffect>::Err(dtype.unwrap_err());
                eff.damage_type = dtype.unwrap().unwrap_or(DamageType::Normal);

                // kind 所需的字段不变量：缺失/为 0 一律加载失败（不静默按 0 结算）
                switch (eff.kind)
                {
                case CardEffectKind::Damage:
                case CardEffectKind::AoeDamage:
                case CardEffectKind::Heal:
                case CardEffectKind::Duel:
                case CardEffectKind::FireAttack:
                    if (eff.amount <= 0)
                        return cfg::fail<CardEffect>(
                            cfg::ConfigErrorKind::InvalidValue,
                            cfg::field_path(path, "amount"));
                    break;
                case CardEffectKind::Draw:
                case CardEffectKind::DiscardTarget:
                    if (eff.count <= 0)
                        return cfg::fail<CardEffect>(
                            cfg::ConfigErrorKind::InvalidValue,
                            cfg::field_path(path, "count"));
                    break;
                case CardEffectKind::Steal:
                    if (eff.count <= 0)
                        return cfg::fail<CardEffect>(
                            cfg::ConfigErrorKind::InvalidValue,
                            cfg::field_path(path, "count"));
                    if (eff.range <= 0)
                        return cfg::fail<CardEffect>(
                            cfg::ConfigErrorKind::InvalidValue,
                            cfg::field_path(path, "range"));
                    break;
                default:
                    break;
                }

                // 火攻必须显式声明火焰伤害：缺失/为普通伤会在运行时静默打普通伤，
                // 加载期直接拒绝（detail 指向 damage_type）
                if (eff.kind == CardEffectKind::FireAttack &&
                    eff.damage_type != DamageType::Fire)
                    return cfg::fail<CardEffect>(
                        cfg::ConfigErrorKind::InvalidValue,
                        cfg::field_path(path, "damage_type"));

                // 铁索连环必须显式声明目标范围：缺失会静默回落 Self 而选不出
                // 1~2 名角色，加载期直接拒绝而非按 0/1 目标错结算
                if (eff.kind == CardEffectKind::Chain && eff.scope.is_none())
                    return cfg::fail<CardEffect>(
                        cfg::ConfigErrorKind::InvalidValue,
                        cfg::field_path(path, "scope"));

                return cfg::ConfigResult<CardEffect>::Ok(std::move(eff));
            }

            cfg::ConfigResult<CardEquip> parse_card_equip(
                const json::Json &obj, std::string_view path)
            {
                CardEquip eq;
                auto slot = require_enum<EquipSlot>(
                    obj, "slot", path,
                    {{"weapon", EquipSlot::Weapon},
                     {"armor", EquipSlot::Armor},
                     {"offensive_horse", EquipSlot::OffensiveHorse},
                     {"defensive_horse", EquipSlot::DefensiveHorse}});
                if (slot.is_err())
                    return cfg::ConfigResult<CardEquip>::Err(slot.unwrap_err());
                eq.slot = slot.unwrap();

                auto range = cfg::opt_int_range(obj, "range", 0, path);
                if (range.is_err())
                    return cfg::ConfigResult<CardEquip>::Err(range.unwrap_err());
                eq.range = range.unwrap();

                return cfg::ConfigResult<CardEquip>::Ok(std::move(eq));
            }

            cfg::ConfigResult<JudgeEffect> parse_judge(
                const json::Json &obj, std::string_view path)
            {
                JudgeEffect j;
                auto trigger = require_enum<JudgeTrigger>(
                    obj, "trigger", path,
                    {{"red", JudgeTrigger::Red},
                     {"black", JudgeTrigger::Black},
                     {"heart", JudgeTrigger::Heart},
                     {"not_heart", JudgeTrigger::NotHeart},
                     {"spade_2_9", JudgeTrigger::Spade2to9},
                     {"not_club", JudgeTrigger::NotClub}});
                if (trigger.is_err())
                    return cfg::ConfigResult<JudgeEffect>::Err(trigger.unwrap_err());
                j.trigger = trigger.unwrap();

                const std::initializer_list<std::pair<std::string_view, JudgeAction>>
                    action_table = {
                        {"nothing", JudgeAction::Nothing},
                        {"skip_play", JudgeAction::SkipPlay},
                        {"damage", JudgeAction::Damage},
                        {"jink", JudgeAction::Jink},
                        {"pass_to_next", JudgeAction::PassToNext},
                        {"skip_draw", JudgeAction::SkipDraw}};

                auto success =
                    require_enum<JudgeAction>(obj, "success", path, action_table);
                if (success.is_err())
                    return cfg::ConfigResult<JudgeEffect>::Err(success.unwrap_err());
                j.success = success.unwrap();

                auto failure =
                    opt_enum<JudgeAction>(obj, "failure", path, action_table);
                if (failure.is_err())
                    return cfg::ConfigResult<JudgeEffect>::Err(failure.unwrap_err());
                j.failure = failure.unwrap().unwrap_or(JudgeAction::Nothing);

                auto amount = cfg::opt_int_range(obj, "amount", 0, path);
                if (amount.is_err())
                    return cfg::ConfigResult<JudgeEffect>::Err(amount.unwrap_err());
                j.amount = amount.unwrap();

                auto scope = opt_enum<Scope>(obj, "scope", path, scope_table);
                if (scope.is_err())
                    return cfg::ConfigResult<JudgeEffect>::Err(scope.unwrap_err());
                j.scope = scope.unwrap();

                auto range = cfg::opt_int_range(obj, "range", 0, path);
                if (range.is_err())
                    return cfg::ConfigResult<JudgeEffect>::Err(range.unwrap_err());
                j.range = range.unwrap();

                auto dtype =
                    opt_enum<DamageType>(obj, "damage_type", path, damage_type_table);
                if (dtype.is_err())
                    return cfg::ConfigResult<JudgeEffect>::Err(dtype.unwrap_err());
                j.damage_type = dtype.unwrap().unwrap_or(DamageType::Normal);

                // Damage 动作的 amount 不变量同 effect 路径：非正一律加载失败
                // （不静默按 0 结算，避免判定卡整局 0 伤无告警运行）
                if ((j.success == JudgeAction::Damage ||
                     j.failure == JudgeAction::Damage) &&
                    j.amount <= 0)
                    return cfg::fail<JudgeEffect>(
                        cfg::ConfigErrorKind::InvalidValue,
                        cfg::field_path(path, "amount"));

                return cfg::ConfigResult<JudgeEffect>::Ok(std::move(j));
            }

            cfg::ConfigResult<std::vector<Ability>> parse_abilities(
                const json::Json &root, std::string_view path)
            {
                std::vector<Ability> out;
                const auto *o = root.try_as_object();
                if (!o)
                    return cfg::fail<std::vector<Ability>>(
                        cfg::ConfigErrorKind::TypeMismatch,
                        cfg::container_path(path));
                if (!o->contains("abilities"))
                    return cfg::ConfigResult<std::vector<Ability>>::Ok(std::move(out));

                const json::Json &v = (*o)["abilities"];
                const auto *arr = v.try_as_array();
                if (!arr)
                    return cfg::fail<std::vector<Ability>>(
                        cfg::ConfigErrorKind::TypeMismatch,
                        cfg::field_path(path, "abilities"));

                const auto prefix = cfg::field_path(path, "abilities");
                for (std::size_t i = 0; i < arr->size(); ++i)
                {
                    const std::string ip = prefix + "[" + std::to_string(i) + "]";
                    auto s = (*arr)[i].try_as_string();
                    if (!s)
                        return cfg::fail<std::vector<Ability>>(
                            cfg::ConfigErrorKind::TypeMismatch, ip);
                    auto a = enum_value<Ability>(*s, ip, ability_table);
                    if (a.is_err())
                        return cfg::ConfigResult<std::vector<Ability>>::Err(
                            a.unwrap_err());
                    out.push_back(a.unwrap());
                }
                return cfg::ConfigResult<std::vector<Ability>>::Ok(std::move(out));
            }

            cfg::ConfigResult<CardDef> parse_card_def(
                const json::Json &root, std::string_view path)
            {
                CardDef def;

                auto id = cfg::require_string(root, "id", path);
                if (id.is_err())
                    return cfg::ConfigResult<CardDef>::Err(id.unwrap_err());
                def.id = id.unwrap();

                auto name = cfg::require_string(root, "name", path);
                if (name.is_err())
                    return cfg::ConfigResult<CardDef>::Err(name.unwrap_err());
                def.name = name.unwrap();

                auto type = require_enum<CardType>(
                    root, "type", path,
                    {{"basic", CardType::Basic}, {"trick", CardType::Trick},
                     {"equipment", CardType::Equipment}});
                if (type.is_err())
                    return cfg::ConfigResult<CardDef>::Err(type.unwrap_err());
                def.type = type.unwrap();

                auto subtype = cfg::opt_string(root, "subtype", "", path);
                if (subtype.is_err())
                    return cfg::ConfigResult<CardDef>::Err(subtype.unwrap_err());
                def.subtype = subtype.unwrap();
                if (!is_valid_subtype(def.subtype))
                    return cfg::fail<CardDef>(
                        cfg::ConfigErrorKind::InvalidValue,
                        cfg::field_path(path, "subtype"));

                auto text = cfg::opt_string(root, "text", "", path);
                if (text.is_err())
                    return cfg::ConfigResult<CardDef>::Err(text.unwrap_err());
                def.text = text.unwrap();

                auto er = cfg::each(root, "copies", path,
                                    [&def](const json::Json &item, std::string_view ip)
                                        -> cfg::ConfigResult<void>
                {
                    auto copy = parse_card_copy(item, ip);
                    if (copy.is_err())
                        return cfg::ConfigResult<void>::Err(copy.unwrap_err());
                    def.copies.push_back(copy.unwrap());
                    return cfg::ConfigResult<void>::Ok();
                });
                if (er.is_err())
                    return cfg::ConfigResult<CardDef>::Err(er.unwrap_err());

                auto effect = opt_object(root, "effect", path);
                if (effect.is_err())
                    return cfg::ConfigResult<CardDef>::Err(effect.unwrap_err());
                if (effect.unwrap().is_some())
                {
                    auto eff = parse_card_effect(
                        *effect.unwrap().unwrap(), cfg::field_path(path, "effect"));
                    if (eff.is_err())
                        return cfg::ConfigResult<CardDef>::Err(eff.unwrap_err());
                    def.effect = Option<CardEffect>::Some(std::move(eff).unwrap());
                }

                auto equip = opt_object(root, "equip", path);
                if (equip.is_err())
                    return cfg::ConfigResult<CardDef>::Err(equip.unwrap_err());
                if (equip.unwrap().is_some())
                {
                    auto eq = parse_card_equip(
                        *equip.unwrap().unwrap(), cfg::field_path(path, "equip"));
                    if (eq.is_err())
                        return cfg::ConfigResult<CardDef>::Err(eq.unwrap_err());
                    def.equip = Option<CardEquip>::Some(std::move(eq).unwrap());
                }

                auto judge = opt_object(root, "judge", path);
                if (judge.is_err())
                    return cfg::ConfigResult<CardDef>::Err(judge.unwrap_err());
                if (judge.unwrap().is_some())
                {
                    auto j = parse_judge(
                        *judge.unwrap().unwrap(), cfg::field_path(path, "judge"));
                    if (j.is_err())
                        return cfg::ConfigResult<CardDef>::Err(j.unwrap_err());
                    def.judge = Option<JudgeEffect>::Some(std::move(j).unwrap());
                }

                auto abilities = parse_abilities(root, path);
                if (abilities.is_err())
                    return cfg::ConfigResult<CardDef>::Err(abilities.unwrap_err());
                def.abilities = std::move(abilities).unwrap();

                auto rescue = cfg::opt_bool(root, "rescue", false, path);
                if (rescue.is_err())
                    return cfg::ConfigResult<CardDef>::Err(rescue.unwrap_err());
                def.rescue = rescue.unwrap();

                auto counter = cfg::opt_bool(root, "counter", false, path);
                if (counter.is_err())
                    return cfg::ConfigResult<CardDef>::Err(counter.unwrap_err());
                def.counter = counter.unwrap();

                auto self_rescue = cfg::opt_bool(root, "self_rescue", false, path);
                if (self_rescue.is_err())
                    return cfg::ConfigResult<CardDef>::Err(self_rescue.unwrap_err());
                def.self_rescue = self_rescue.unwrap();

                auto recast = cfg::opt_bool(root, "recast", false, path);
                if (recast.is_err())
                    return cfg::ConfigResult<CardDef>::Err(recast.unwrap_err());
                def.recast = recast.unwrap();

                return cfg::ConfigResult<CardDef>::Ok(std::move(def));
            }
        }

        Option<CardEffectKind> effect_kind_from_name(std::string_view name)
        {
            for (const auto &[key, val] : detail::effect_kind_table)
                if (key == name)
                    return Option<CardEffectKind>::Some(val);
            return Option<CardEffectKind>::None();
        }

        Option<Ability> ability_from_name(std::string_view name)
        {
            for (const auto &[key, val] : detail::ability_table)
                if (key == name)
                    return Option<Ability>::Some(val);
            return Option<Ability>::None();
        }

        cfg::ConfigResult<std::vector<RawMechanisms>> scan_mechanisms(
            const cfg::ResourceStore &store, std::string_view deck_name)
        {
            auto deck_doc = store.load(deck_name);
            if (deck_doc.is_err())
                return cfg::ConfigResult<std::vector<RawMechanisms>>::Err(
                    deck_doc.unwrap_err());
            const json::Json &root = deck_doc.unwrap().root();

            std::vector<RawMechanisms> out;
            auto er = cfg::each(root, "cards", {},
                                [&out, &store](const json::Json &item,
                                               std::string_view ip)
                                    -> cfg::ConfigResult<void>
            {
                auto id_s = item.try_as_string();
                if (!id_s)
                    return cfg::fail<void>(
                        cfg::ConfigErrorKind::TypeMismatch, std::string(ip));

                RawMechanisms raw;
                raw.id = std::string(*id_s);
                raw.name = raw.id;

                const std::string file = "cards/" + raw.id + ".json";
                auto card_doc = store.load("cards/" + raw.id);
                if (card_doc.is_err())
                    return cfg::ConfigResult<void>::Err(card_doc.unwrap_err());
                const json::Json &card = card_doc.unwrap().root();
                const auto *co = card.try_as_object();
                if (!co)
                    return cfg::fail<void>(cfg::ConfigErrorKind::TypeMismatch, file);

                auto name = cfg::opt_string(card, "name", raw.id, file);
                if (name.is_err())
                    return cfg::ConfigResult<void>::Err(name.unwrap_err());
                raw.name = name.unwrap();

                // 只取 effect.kind 原文；effect 缺失或为非对象按同级字段错误处理
                if (co->contains("effect"))
                {
                    const auto *eo = (*co)["effect"].try_as_object();
                    if (!eo)
                        return cfg::fail<void>(
                            cfg::ConfigErrorKind::TypeMismatch,
                            cfg::field_path(file, "effect"));
                    if (eo->contains("kind"))
                    {
                        auto k = (*eo)["kind"].try_as_string();
                        if (!k)
                            return cfg::fail<void>(
                                cfg::ConfigErrorKind::TypeMismatch,
                                cfg::field_path(file, "effect.kind"));
                        raw.effect_kind = Option<std::string>::Some(std::string(*k));
                    }
                }

                // 只取 abilities 原文；数组元素非字符串按同级字段错误处理
                if (co->contains("abilities"))
                {
                    const auto *ao = (*co)["abilities"].try_as_array();
                    if (!ao)
                        return cfg::fail<void>(
                            cfg::ConfigErrorKind::TypeMismatch,
                            cfg::field_path(file, "abilities"));
                    const std::string prefix = cfg::field_path(file, "abilities");
                    for (std::size_t i = 0; i < ao->size(); ++i)
                    {
                        const auto s = (*ao)[i].try_as_string();
                        if (!s)
                            return cfg::fail<void>(
                                cfg::ConfigErrorKind::TypeMismatch,
                                prefix + "[" + std::to_string(i) + "]");
                        raw.abilities.push_back(std::string(*s));
                    }
                }

                out.push_back(std::move(raw));
                return cfg::ConfigResult<void>::Ok();
            });
            if (er.is_err())
                return cfg::ConfigResult<std::vector<RawMechanisms>>::Err(
                    er.unwrap_err());
            return cfg::ConfigResult<std::vector<RawMechanisms>>::Ok(std::move(out));
        }

        CardDefCatalog::CardDefCatalog(CardDefCatalog &&other) noexcept
            : defs(std::move(other.defs)), index(std::move(other.index))
        {
        }

        CardDefCatalog &CardDefCatalog::operator=(CardDefCatalog &&other) noexcept
        {
            defs = std::move(other.defs);
            index = std::move(other.index);
            return *this;
        }

        cfg::ConfigResult<CardDefCatalog> CardDefCatalog::load(
            const cfg::ResourceStore &store, std::string_view deck_name)
        {
            auto deck_doc = store.load(deck_name);
            if (deck_doc.is_err())
                return cfg::ConfigResult<CardDefCatalog>::Err(deck_doc.unwrap_err());
            const json::Json &root = deck_doc.unwrap().root();

            CardDefCatalog catalog;
            auto er = cfg::each(root, "cards", {},
                                [&catalog, &store](const json::Json &item,
                                                   std::string_view ip)
                                    -> cfg::ConfigResult<void>
            {
                auto id_s = item.try_as_string();
                if (!id_s)
                    return cfg::fail<void>(
                        cfg::ConfigErrorKind::TypeMismatch, std::string(ip));
                const std::string cid(*id_s);

                if (catalog.index.find(cid) != catalog.index.end())
                    return cfg::fail<void>(
                        cfg::ConfigErrorKind::InvalidValue,
                        std::string(ip) + " 重复引用卡牌 " + cid);

                const std::string file = "cards/" + cid + ".json";
                auto card_doc = store.load("cards/" + cid);
                if (card_doc.is_err())
                    return cfg::ConfigResult<void>::Err(card_doc.unwrap_err());

                auto def = detail::parse_card_def(card_doc.unwrap().root(), file);
                if (def.is_err())
                    return cfg::ConfigResult<void>::Err(def.unwrap_err());

                if (def.unwrap().id != cid)
                    return cfg::fail<void>(
                        cfg::ConfigErrorKind::InvalidValue, file + ".id");

                catalog.index.emplace(cid, catalog.defs.size());
                catalog.defs.push_back(std::move(def).unwrap());
                return cfg::ConfigResult<void>::Ok();
            });
            if (er.is_err())
                return cfg::ConfigResult<CardDefCatalog>::Err(er.unwrap_err());

            // 牌堆不变量：总张数为 0（空引用或全 0 副本）不得建成目录——
            // 空牌堆只会空转到最大回合数产出误导性平局，加载期即拒
            if (catalog.total_copies() == 0)
                return cfg::fail<CardDefCatalog>(
                    cfg::ConfigErrorKind::InvalidValue,
                    std::string(deck_name) + ".json.cards: 牌堆为空（无张数 > 0 的卡）");
            return cfg::ConfigResult<CardDefCatalog>::Ok(std::move(catalog));
        }

        Option<const CardDef *> CardDefCatalog::find(const std::string &id) const
        {
            auto it = index.find(id);
            if (it == index.end())
                return Option<const CardDef *>::None();
            return Option<const CardDef *>::Some(&defs[it->second]);
        }

        std::size_t CardDefCatalog::total_copies() const noexcept
        {
            std::size_t n = 0;
            for (const auto &def : defs)
                n += def.copies.size();
            return n;
        }

        const std::string &display_name(const CardDef &def) noexcept
        {
            return def.name.empty() ? def.id : def.name;
        }

        std::string display_name(
            const CardDefCatalog &catalog, const std::string &def_id)
        {
            const auto def = catalog.find(def_id);
            return def.is_some() ? display_name(*def.unwrap()) : def_id;
        }

        std::string display_name(
            const CardDefCatalog *catalog, const std::string &def_id)
        {
            return catalog == nullptr ? def_id : display_name(*catalog, def_id);
        }
    }
}
