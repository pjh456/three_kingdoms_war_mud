/**
 * @file   catalog.cpp
 * @brief  武将目录 `HeroCatalog` 加载与技能/性别解析的定义。
 * @ingroup tkw_hero
 */

#include "hero/catalog.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tkw
{
    namespace hero
    {
        namespace detail
        {
            cfg::ConfigResult<HeroSkill> skill_value(
                std::string_view s, std::string_view path)
            {
                for (const auto &[key, val] : skill_table)
                    if (key == s)
                        return cfg::ConfigResult<HeroSkill>::Ok(val);
                return cfg::fail<HeroSkill>(
                    cfg::ConfigErrorKind::InvalidValue, std::string(path));
            }

            cfg::ConfigResult<std::vector<HeroSkill>> parse_skills(
                const json::Json &root, std::string_view path)
            {
                std::vector<HeroSkill> out;
                const auto *o = root.try_as_object();
                if (!o)
                    return cfg::fail<std::vector<HeroSkill>>(
                        cfg::ConfigErrorKind::TypeMismatch,
                        cfg::container_path(path));
                if (!o->contains("skills"))
                    return cfg::ConfigResult<std::vector<HeroSkill>>::Ok(
                        std::move(out));

                const json::Json &v = (*o)["skills"];
                const auto *arr = v.try_as_array();
                if (!arr)
                    return cfg::fail<std::vector<HeroSkill>>(
                        cfg::ConfigErrorKind::TypeMismatch,
                        cfg::field_path(path, "skills"));

                const auto prefix = cfg::field_path(path, "skills");
                for (std::size_t i = 0; i < arr->size(); ++i)
                {
                    const std::string ip =
                        prefix + "[" + std::to_string(i) + "]";
                    auto s = (*arr)[i].try_as_string();
                    if (!s)
                        return cfg::fail<std::vector<HeroSkill>>(
                            cfg::ConfigErrorKind::TypeMismatch, ip);
                    auto skill = skill_value(*s, ip);
                    if (skill.is_err())
                        return cfg::ConfigResult<std::vector<HeroSkill>>::Err(
                            skill.unwrap_err());
                    out.push_back(skill.unwrap());
                }
                return cfg::ConfigResult<std::vector<HeroSkill>>::Ok(std::move(out));
            }

            cfg::ConfigResult<Option<entity::Gender>> parse_gender(
                const json::Json &root, std::string_view path)
            {
                const auto *o = root.try_as_object();
                if (!o)
                    return cfg::fail<Option<entity::Gender>>(
                        cfg::ConfigErrorKind::TypeMismatch,
                        cfg::container_path(path));
                if (!o->contains("gender"))
                    return cfg::ConfigResult<Option<entity::Gender>>::Ok(
                        Option<entity::Gender>::None());

                auto s = (*o)["gender"].try_as_string();
                if (!s)
                    return cfg::fail<Option<entity::Gender>>(
                        cfg::ConfigErrorKind::TypeMismatch,
                        cfg::field_path(path, "gender"));

                entity::Gender g = entity::Gender::Male;
                if (*s == "male")
                    g = entity::Gender::Male;
                else if (*s == "female")
                    g = entity::Gender::Female;
                else
                    return cfg::fail<Option<entity::Gender>>(
                        cfg::ConfigErrorKind::InvalidValue,
                        cfg::field_path(path, "gender"));
                return cfg::ConfigResult<Option<entity::Gender>>::Ok(
                    Option<entity::Gender>::Some(g));
            }

            cfg::ConfigResult<HeroDef> parse_hero_def(
                const json::Json &root, std::string_view path)
            {
                HeroDef def;

                auto id = cfg::require_string(root, "id", path);
                if (id.is_err())
                    return cfg::ConfigResult<HeroDef>::Err(id.unwrap_err());
                def.id = id.unwrap();

                auto name = cfg::require_string(root, "name", path);
                if (name.is_err())
                    return cfg::ConfigResult<HeroDef>::Err(name.unwrap_err());
                def.name = name.unwrap();

                auto gender = parse_gender(root, path);
                if (gender.is_err())
                    return cfg::ConfigResult<HeroDef>::Err(gender.unwrap_err());
                def.gender = gender.unwrap();

                auto hp = cfg::opt_int_range(root, "hp", 0, path);
                if (hp.is_err())
                    return cfg::ConfigResult<HeroDef>::Err(hp.unwrap_err());
                if (hp.unwrap() < 0)
                    return cfg::fail<HeroDef>(
                        cfg::ConfigErrorKind::InvalidValue,
                        cfg::field_path(path, "hp"));
                def.hp = hp.unwrap();

                auto skills = parse_skills(root, path);
                if (skills.is_err())
                    return cfg::ConfigResult<HeroDef>::Err(skills.unwrap_err());
                def.skills = std::move(skills).unwrap();

                auto text = cfg::opt_string(root, "text", "", path);
                if (text.is_err())
                    return cfg::ConfigResult<HeroDef>::Err(text.unwrap_err());
                def.text = text.unwrap();

                return cfg::ConfigResult<HeroDef>::Ok(std::move(def));
            }
        }  // namespace detail

        HeroCatalog::HeroCatalog(HeroCatalog &&other) noexcept
            : defs(std::move(other.defs)), index(std::move(other.index))
        {
        }

        HeroCatalog &HeroCatalog::operator=(HeroCatalog &&other) noexcept
        {
            defs = std::move(other.defs);
            index = std::move(other.index);
            return *this;
        }

        cfg::ConfigResult<HeroCatalog> HeroCatalog::load(
            const cfg::ResourceStore &store, std::string_view hero_name)
        {
            auto doc = store.load(hero_name);
            if (doc.is_err())
                return cfg::ConfigResult<HeroCatalog>::Err(doc.unwrap_err());
            const json::Json &root = doc.unwrap().root();

            HeroCatalog catalog;
            auto er = cfg::each(root, "heroes", {},
                                [&catalog, &store](const json::Json &item,
                                                   std::string_view ip)
                                    -> cfg::ConfigResult<void>
            {
                auto id_s = item.try_as_string();
                if (!id_s)
                    return cfg::fail<void>(
                        cfg::ConfigErrorKind::TypeMismatch, std::string(ip));
                const std::string hid(*id_s);

                if (catalog.index.find(hid) != catalog.index.end())
                    return cfg::fail<void>(
                        cfg::ConfigErrorKind::InvalidValue,
                        std::string(ip) + " 重复引用武将 " + hid);

                const std::string file = "heroes/" + hid + ".json";
                auto hero_doc = store.load("heroes/" + hid);
                if (hero_doc.is_err())
                    return cfg::ConfigResult<void>::Err(hero_doc.unwrap_err());

                auto def = detail::parse_hero_def(hero_doc.unwrap().root(), file);
                if (def.is_err())
                    return cfg::ConfigResult<void>::Err(def.unwrap_err());

                if (def.unwrap().id != hid)
                    return cfg::fail<void>(
                        cfg::ConfigErrorKind::InvalidValue, file + ".id");

                catalog.index.emplace(hid, catalog.defs.size());
                catalog.defs.push_back(std::move(def).unwrap());
                return cfg::ConfigResult<void>::Ok();
            });
            if (er.is_err())
                return cfg::ConfigResult<HeroCatalog>::Err(er.unwrap_err());
            return cfg::ConfigResult<HeroCatalog>::Ok(std::move(catalog));
        }

        cfg::ConfigResult<HeroCatalog> HeroCatalog::load_optional(
            const cfg::ResourceStore &store, std::string_view hero_name)
        {
            if (!tkw::io::exists(store.root() /
                                 (std::string(hero_name) + ".json")))
                return cfg::ConfigResult<HeroCatalog>::Ok(HeroCatalog{});
            return load(store, hero_name);
        }

        Option<const HeroDef *> HeroCatalog::find(const std::string &id) const
        {
            auto it = index.find(id);
            if (it == index.end())
                return Option<const HeroDef *>::None();
            return Option<const HeroDef *>::Some(&defs[it->second]);
        }

        std::string display_hero_name(
            const HeroCatalog &catalog, const std::string &hero_id)
        {
            const auto def = catalog.find(hero_id);
            return def.is_some() ? display_hero_name(*def.unwrap()) : hero_id;
        }

        std::string display_hero_name(
            const HeroCatalog *catalog, const std::string &hero_id)
        {
            return catalog == nullptr ? hero_id
                                      : display_hero_name(*catalog, hero_id);
        }
    }
}
