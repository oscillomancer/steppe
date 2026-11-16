// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include <doctest/doctest.h>
#include <supple/cmd/bus.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>

namespace supple::test {
// Mutate the registered contract, including optional fields, nested objects,
// array elements, local references and every union alternative. Accepted inputs
// are NOT executed: a schema can intentionally accept arbitrary plug-in JSON or
// a real device/file action. The caller checks rejected inputs at the dispatcher.
class CommandInputs {
  public:
    using Visit = std::function<void(const json::Value&, const std::string&)>;
    explicit CommandInputs(std::uint64_t seed) : state_(seed) {}
    void generate(const json::Value& schema, const Visit& visit) {
        walk(schema, schema, {}, [](json::Value v) { return v; }, visit, 0);
    }
    // A deliberately conservative, independent contract oracle. Unknown
    // keywords (e.g. unit-string patterns) are allowed. A definite type/range/
    // required-member violation must never become accepted by a broken decoder.
    bool allows(const json::Value& root, const json::Value& value) const {
        return allows(root, root, value, 0);
    }

  private:
    using Wrap = std::function<json::Value(json::Value)>;
    std::uint64_t state_;
    std::uint64_t next() {
        auto x = (state_ += 0x9e3779b97f4a7c15ULL);
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
        return x ^ (x >> 31);
    }
    const json::Value& deref(const json::Value& root, const json::Value& s) const {
        if (const auto* ref = s.find("$ref"); ref && ref->isString()) {
            const auto* resolved = json::resolveFragment(root, ref->asString());
            REQUIRE_MESSAGE(resolved, "unresolved registered schema reference " << ref->asString());
            return *resolved;
        }
        return s;
    }
    bool allows(const json::Value& root, const json::Value& raw, const json::Value& v,
                unsigned depth) const {
        REQUIRE(depth < 32);
        const auto& s = deref(root, raw);
        if (s.isBool()) return s.asBool();
        // optional<T> deliberately accepts null as absent, although its export
        // describes T alone. Keep probing null through the real decoder; this
        // partial oracle cannot distinguish optional fields from required ones.
        if (v.isNull()) return true;
        for (const char* key : {"anyOf", "oneOf", "allOf"})
            if (const auto* choices = s.find(key); choices && choices->isArray()) {
                std::size_t matched = 0;
                for (const auto& child : choices->asArray())
                    if (allows(root, child, v, depth + 1)) ++matched;
                // Treat oneOf as anyOf: omitted pattern constraints can make
                // more than one branch appear to match in this partial oracle.
                if (std::string_view(key) == "allOf" ? matched != choices->size() : matched == 0)
                    return false;
            }
        if (const auto* choices = s.find("enum"); choices && choices->isArray())
            if (std::find(choices->asArray().begin(), choices->asArray().end(), v) ==
                choices->asArray().end())
                return false;
        if (const auto* c = s.find("const"); c && *c != v) return false;
        if (const auto* type = s.find("type"); type && type->isString()) {
            const auto& t = type->asString();
            if ((t == "object" && !v.isObject()) || (t == "array" && !v.isArray()) ||
                (t == "string" && !v.isString()) || (t == "boolean" && !v.isBool()) ||
                (t == "null" && !v.isNull()) || (t == "number" && !v.isNumber()) ||
                (t == "integer" && (!v.isNumber() || std::trunc(v.asDouble()) != v.asDouble())))
                return false;
        }
        if (v.isNumber())
            for (const char* key : {"minimum", "maximum", "exclusiveMinimum", "exclusiveMaximum"})
                if (const auto* bound = s.find(key); bound && bound->isNumber()) {
                    const double x = v.asDouble(), b = bound->asDouble();
                    if ((std::string_view(key) == "minimum" && x < b) ||
                        (std::string_view(key) == "maximum" && x > b) ||
                        (std::string_view(key) == "exclusiveMinimum" && x <= b) ||
                        (std::string_view(key) == "exclusiveMaximum" && x >= b))
                        return false;
                }
        if (v.isObject()) {
            if (const auto* required = s.find("required"); required && required->isArray())
                for (const auto& key : required->asArray())
                    if (key.isString() && !v.find(key.asString())) return false;
            const auto* properties = s.find("properties");
            for (const auto& field : v.asObject()) {
                const auto* child = properties ? properties->find(field.key) : nullptr;
                if (!child) child = s.find("additionalProperties");
                if (child && !allows(root, *child, field.value, depth + 1)) return false;
            }
        }
        if (v.isArray())
            if (const auto* items = s.find("items"))
                for (const auto& item : v.asArray())
                    if (!allows(root, *items, item, depth + 1)) return false;
        return true;
    }
    json::Value sample(const json::Value& root, const json::Value& raw, unsigned depth) const {
        REQUIRE_MESSAGE(depth < 32, "recursive command schema needs an explicit fixture");
        const auto& s = deref(root, raw);
        if (const auto* e = s.find("enum"); e && e->isArray() && e->size()) return e->at(0);
        if (const auto* c = s.find("const")) return *c;
        for (const char* key : {"anyOf", "oneOf"})
            if (const auto* choices = s.find(key); choices && choices->isArray() && choices->size())
                return sample(root, choices->at(0), depth + 1);
        const auto* t = s.find("type");
        const std::string type = t && t->isString() ? t->asString() : "";
        if (type == "object" || s.find("properties")) {
            auto out = json::Value::object();
            const auto* properties = s.find("properties");
            if (const auto* required = s.find("required");
                required && required->isArray() && properties && properties->isObject())
                for (const auto& key : required->asArray()) {
                    REQUIRE(key.isString());
                    const auto* field = properties->find(key.asString());
                    REQUIRE(field);
                    out[key.asString()] = sample(root, *field, depth + 1);
                }
            return out;
        }
        if (type == "array") {
            auto out = json::Value::array();
            if (const auto* items = s.find("items")) out.push_back(sample(root, *items, depth + 1));
            return out;
        }
        if (type == "string") return "fuzz";
        if (type == "boolean") return false;
        if (type == "null") return nullptr;
        if (const auto* min = s.find("minimum"); min && min->isNumber()) return *min;
        return 0;
    }
    void walk(const json::Value& root, const json::Value& raw, const std::string& path,
              const Wrap& wrap, const Visit& visit, unsigned depth) {
        REQUIRE(depth < 32);
        const auto& s = deref(root, raw);
        std::vector<json::Value> values{nullptr,
                                        false,
                                        true,
                                        0,
                                        -1,
                                        1.5,
                                        "invalid-choice",
                                        json::Value::array(),
                                        json::Value::object(),
                                        std::numeric_limits<long long>::min(),
                                        std::numeric_limits<long long>::max(),
                                        -0x1p63,
                                        0x1p63,
                                        -std::numeric_limits<double>::max(),
                                        std::numeric_limits<double>::max()};
        for (const char* key : {"minimum", "maximum", "exclusiveMinimum", "exclusiveMaximum"})
            if (const auto* bound = s.find(key); bound && bound->isNumber()) {
                const double x = bound->asDouble();
                const double step = std::max(1.0, std::abs(x) * .01);
                values.emplace_back(x);
                if (std::isfinite(x - step)) values.emplace_back(x - step);
                if (std::isfinite(x + step)) values.emplace_back(x + step);
            }
        // Stable Fisher-Yates, independent of the standard library's shuffle.
        for (std::size_t n = values.size(); n > 1; --n)
            std::swap(values[n - 1], values[static_cast<std::size_t>(next() % n)]);
        for (auto& value : values) visit(wrap(std::move(value)), path);
        for (const char* key : {"anyOf", "oneOf", "allOf"})
            if (const auto* choices = s.find(key); choices && choices->isArray())
                for (const auto& child : choices->asArray())
                    walk(root, child, path, wrap, visit, depth + 1);
        auto base = sample(root, s, depth);
        if (const auto* fields = s.find("properties"); fields && fields->isObject()) {
            REQUIRE(base.isObject());
            for (const auto& field : fields->asObject()) {
                auto childPath = json::pointerJoin(path, field.key);
                walk(
                    root, field.value, childPath,
                    [base, key = field.key, wrap](json::Value v) mutable {
                        auto out = base;
                        out[key] = std::move(v);
                        return wrap(std::move(out));
                    },
                    visit, depth + 1);
            }
            if (const auto* required = s.find("required"); required && required->isArray())
                for (const auto& key : required->asArray()) {
                    REQUIRE(key.isString());
                    auto out = base;
                    out.asObject().erase(key.asString());
                    visit(wrap(std::move(out)), json::pointerJoin(path, key.asString()));
                }
        }
        if (const auto* items = s.find("items"); items && base.isArray() && base.size())
            walk(
                root, *items, json::pointerJoin(path, "0"),
                [base, wrap](json::Value v) mutable {
                    auto out = base;
                    out.asArray()[0] = std::move(v);
                    return wrap(std::move(out));
                },
                visit, depth + 1);
        if (const auto* extra = s.find("additionalProperties");
            extra && extra->isObject() && base.isObject())
            walk(
                root, *extra, json::pointerJoin(path, "fuzz-key"),
                [base, wrap](json::Value v) mutable {
                    auto out = base;
                    out["fuzz-key"] = std::move(v);
                    return wrap(std::move(out));
                },
                visit, depth + 1);
    }
};
}  // namespace supple::test
