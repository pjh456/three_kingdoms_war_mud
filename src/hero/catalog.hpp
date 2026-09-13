/**
 * @file catalog.hpp
 * @brief 武将目录：heroes.json + heroes/<id>.json 的加载与语义校验。
 * @note 语义校验归本域（config 只管「文件 → Document」）：
 *       - heroes.json 引用一名武将 → 按 <root>/heroes/<id>.json 加载单武将文件；
 *       - 未知技能名 / 未知性别在加载时立即 InvalidValue 失败（detail 为字段路径）；
 *       - 文件内 id 必须等于文件名（引用方），不一致即数据事故。
 * @note 加载完成后不持有 Document：全部解析成 HeroDef 值类型，Document 即弃。
 * @note 武将目录独立于牌表文件，不参与 deck_hash；缺 heroes.json 时经
 *       load_optional 回落空目录，使无武将数据的自定义牌表照常可玩。
 */

#ifndef INCLUDE_TKW_HERO_CATALOG_HPP
#define INCLUDE_TKW_HERO_CATALOG_HPP

#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "config/error.hpp"
#include "config/fields.hpp"
#include "config/resource.hpp"
#include "hero/def.hpp"
#include "io/file.hpp"
#include "util/types.hpp"

namespace tkw
{
    namespace hero
    {
        namespace json = pjh::json;
        namespace cfg = tkw::config;

        namespace detail
        {
            /** skills 封闭名表：严格解析与展示名查询共用的单一表源。 */
            inline constexpr
                std::initializer_list<std::pair<std::string_view, HeroSkill>>
                    skill_table{{"paoxiao", HeroSkill::PaoXiao},
                                {"wusheng", HeroSkill::WuSheng},
                                {"yingzi", HeroSkill::YingZi},
                                {"fankui", HeroSkill::FanKui},
                                {"mashu", HeroSkill::MaShu},
                                {"qicai", HeroSkill::QiCai},
                                {"longdan", HeroSkill::LongDan}};

            /** 字符串 → 技能枚举：未知值报 InvalidValue（detail = 字段路径）。 */
            inline cfg::ConfigResult<HeroSkill> skill_value(
                std::string_view s, std::string_view path)
            {
                for (const auto &[key, val] : skill_table)
                    if (key == s)
                        return cfg::ConfigResult<HeroSkill>::Ok(val);
                return cfg::fail<HeroSkill>(
                    cfg::ConfigErrorKind::InvalidValue, std::string(path));
            }

            /** 解析 skills 数组（缺省 = 空；未知技能名报 InvalidValue）。 */
            inline cfg::ConfigResult<std::vector<HeroSkill>> parse_skills(
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

            /** 解析可选性别：缺失回落 None；未知文本报 InvalidValue。 */
            inline cfg::ConfigResult<Option<entity::Gender>> parse_gender(
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

            /**
             * @brief 解析单武将文件（root = 文件顶层对象）。
             * @param path 容器路径（如 "heroes/zhangfei.json"），用于拼错误字段路径。
             */
            inline cfg::ConfigResult<HeroDef> parse_hero_def(
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

        /** @brief 技能中文展示名（技能表/警告文案用）。 */
        inline constexpr const char *display_skill_name(HeroSkill skill)
        {
            switch (skill)
            {
            case HeroSkill::PaoXiao:
                return "咆哮";
            case HeroSkill::WuSheng:
                return "武圣";
            case HeroSkill::YingZi:
                return "英姿";
            case HeroSkill::FanKui:
                return "反馈";
            case HeroSkill::MaShu:
                return "马术";
            case HeroSkill::QiCai:
                return "奇才";
            case HeroSkill::LongDan:
                return "龙胆";
            }
            return "";
        }

        /**
         * @class HeroCatalog
         * @brief 对局作用域的武将定义容器：heroes.json 引用的有序定义 + id 索引。
         * @note 一次加载后不可变；迭代顺序 = heroes.json 引用顺序，id 查询走内部
         *       索引（O(1)）。
         */
        class HeroCatalog
        {
        public:
            /**
             * @brief 从 ResourceStore 加载：先读 <hero_name>.json（武将构成），
             *        再逐个加载 heroes/<id>.json。
             * @return Ok 为目录；Err 为 config 层错误（文件缺失/非法 JSON/
             *         MissingField/TypeMismatch/InvalidValue，detail 带定位）。
             */
            static cfg::ConfigResult<HeroCatalog> load(
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

            /**
             * @brief 可选加载：<hero_name>.json 不存在时回落空目录；其余错误透传。
             * @return Ok 为目录（缺文件时为空）；Err 为解析/类型/枚举等硬错误。
             * @note 建局与 `tkw heroes` 共用此入口：无武将数据的自定义牌表不新增
             *       失败面，坏数据仍在加载期硬失败。
             */
            static cfg::ConfigResult<HeroCatalog> load_optional(
                const cfg::ResourceStore &store, std::string_view hero_name)
            {
                if (!tkw::io::exists(store.root() /
                                     (std::string(hero_name) + ".json")))
                    return cfg::ConfigResult<HeroCatalog>::Ok(HeroCatalog{});
                return load(store, hero_name);
            }

            /** @brief O(1) 按武将 id 查询（经内部索引）；不存在时为 None。 */
            Option<const HeroDef *> find(const std::string &id) const
            {
                auto it = index.find(id);
                if (it == index.end())
                    return Option<const HeroDef *>::None();
                return Option<const HeroDef *>::Some(&defs[it->second]);
            }

            std::size_t size() const noexcept { return defs.size(); }
            bool empty() const noexcept { return defs.empty(); }

            /** @brief 按 heroes.json 引用顺序迭代。 */
            auto begin() const noexcept { return defs.begin(); }
            auto end() const noexcept { return defs.end(); }

        private:
            std::vector<HeroDef> defs; /**< heroes.json 引用顺序 */
            std::unordered_map<std::string, std::size_t> index; /**< id → defs 下标 */
        };

        /**
         * @brief 按 id 从目录取展示名。
         * @return 目录收录且 name 非空 → name；否则回落 hero_id 本身（拷贝）。
         * @note 结果按值返回：目录未收录时返回入参的拷贝，不暴露调用方引用。
         */
        inline std::string display_hero_name(
            const HeroCatalog &catalog, const std::string &hero_id)
        {
            const auto def = catalog.find(hero_id);
            return def.is_some() ? display_hero_name(*def.unwrap()) : hero_id;
        }

        /**
         * @brief 目录指针可空（无武将数据）的展示名。
         * @return catalog 为空或未收录 → hero_id；否则同目录重载。
         */
        inline std::string display_hero_name(
            const HeroCatalog *catalog, const std::string &hero_id)
        {
            return catalog == nullptr ? hero_id
                                      : display_hero_name(*catalog, hero_id);
        }
    }
}

#endif  // INCLUDE_TKW_HERO_CATALOG_HPP
