// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#pragma once
#include <supple/base/closest.h>
#include <supple/base/units.h>
#include <supple/json/json.h>
#include <algorithm>
#include <any>
#include <cmath>
#include <compare>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <span>
#include <type_traits>
#include <variant>
namespace supple::schema {
using units::dimensionName;
using units::formatQuantity;
using units::parseQuantity;
using units::Unit;
using units::unitName;
enum class UnknownMembers { Error, Ignore };
struct DecodeOptions {
    UnknownMembers unknownMembers = UnknownMembers::Error;
    bool acceptUnitStrings = true;
    bool coerceIntegralDoubles = true;
    std::size_t maxErrors = 50;
};
struct Warning {
    std::string code, message, path;
};
struct CheckCtx {
    std::vector<Error>* errors = nullptr;
    std::string path;
    std::size_t maxErrors = 50;
    void error(std::string code, std::string message, std::string hint) {
        if (errors && errors->size() < maxErrors)
            errors->emplace_back(std::move(code), std::move(message), path, std::move(hint));
    }
};
struct DecodeCtx {
    DecodeOptions options;
    std::vector<Error> errors;
    std::vector<Warning>* warnings = nullptr;
    void error(std::string code, std::string message, std::string path, std::string hint = {}) {
        if (errors.size() < options.maxErrors)
            errors.emplace_back(std::move(code), std::move(message), std::move(path),
                                std::move(hint));
    }
};
inline std::string quantityPattern(Unit unit) {
    std::string suffix;
    switch (unit) {
        case Unit::Hz:
            suffix = "(?:[hH][zZ]|[kK][hH][zZ]|[kK])";
            break;
        case Unit::ms:
            suffix = "(?:ms|s|sec|us|µs)";
            break;
        case Unit::dB:
            suffix = "(?:dB|db)";
            break;
        case Unit::LUFS:
            suffix = "(?:LUFS|LKFS)";
            break;
        case Unit::LU:
            suffix = "LU";
            break;
        case Unit::st:
            suffix = "(?:st|semi|semitones)";
            break;
        case Unit::ct:
            suffix = "(?:ct|cent|cents)";
            break;
        case Unit::oct:
            suffix = "(?:oct|octave|octaves)";
            break;
        case Unit::percent:
            suffix = "%";
            break;
        case Unit::ratio:
            suffix = "(?::1|ratio)";
            break;
        case Unit::x:
            suffix = "(?:x|×)";
            break;
        case Unit::deg:
            suffix = "(?:deg|°)";
            break;
        case Unit::bpm:
            suffix = "bpm";
            break;
        case Unit::beats:
            suffix = "(?:beats|qn)";
            break;
        case Unit::samples:
            suffix = "(?:samples|smp)";
            break;
        case Unit::m:
            suffix = "(?:m|meters|metres|cm|ft)";
            break;
        case Unit::none:
            suffix.clear();
            break;
    }
    const std::string number = "[+-]?(?:[0-9]+(?:\\.[0-9]*)?|\\.[0-9]+)(?:[eE][+-]?[0-9]+)?";
    return "^\\s*" + number + (suffix.empty() ? "" : "(?:\\s*" + suffix + ")?") + "\\s*$";
}
inline json::Value generatedExample(const json::Value& exported) {
    if (const auto* choices = exported.find("enum");
        choices && choices->isArray() && choices->size())
        return choices->at(0);
    const auto* type = exported.find("type");
    if (type && type->isString()) {
        if (type->asString() == "boolean") return true;
        if (type->asString() == "string") return "text";
        if (type->asString() == "array") return json::Value::array();
        if (type->asString() == "object") return json::Value::object();
        if (type->asString() == "integer") {
            if (const auto* minimum = exported.find("minimum"))
                return static_cast<long long>(minimum->asInt());
            return 0;
        }
    }
    if (const auto* minimum = exported.find("minimum")) return *minimum;
    return 0.0;
}
inline std::string generatedExampleText(const json::Value& exported) {
    const auto* type = exported.find("type");
    if (type && type->isString()) {
        if (type->asString() == "object") return "{…}";
        if (type->asString() == "array") return "[…]";
    }
    return json::writeCompact(generatedExample(exported));
}
inline std::string listNames(std::span<const std::string_view> names) {
    std::string result;
    for (std::size_t i = 0; i < std::min<std::size_t>(names.size(), 12); ++i) {
        if (!result.empty()) result += ", ";
        result += names[i];
    }
    if (names.size() > 12) result += ", …";
    return result;
}
template <class T>
struct IsOptional : std::false_type {};
template <class T>
struct IsOptional<std::optional<T>> : std::true_type {};

template <class T>
class Schema {
  public:
    using Decoder = std::function<bool(const json::Value&, T&, DecodeCtx&, const std::string&)>;
    using Encoder = std::function<json::Value(const T&)>;
    Schema() = default;
    Schema(std::string title, Decoder d, Encoder e, json::Value s)
        : title_(std::move(title)),
          decoder_(std::move(d)),
          encoder_(std::move(e)),
          export_(std::move(s)) {}
    bool decodeInto(const json::Value& v, T& out, DecodeCtx& c, const std::string& path) const {
        const auto previousErrors = c.errors.size();
        bool ok = false;
        if constexpr (std::is_floating_point_v<T>) {
            if (unit_ && v.isString() && c.options.acceptUnitStrings) {
                auto q = parseQuantity(v.asString(), *unit_);
                if (q) {
                    out = static_cast<T>(*q);
                    ok = true;
                } else {
                    auto e = q.error();
                    c.error(e.code, e.message, path, e.hint);
                    return false;
                }
            } else
                ok = decoder_(v, out, c, path);
        } else
            ok = decoder_(v, out, c, path);
        if (!ok) {
            for (std::size_t i = previousErrors; i < c.errors.size(); ++i)
                if (c.errors[i].code == "schema.type" && c.errors[i].path == path) {
                    c.errors[i].hint =
                        "expected " + title_ + "; example: " +
                        (example_ ? json::writeCompact(*example_) : generatedExampleText(export_));
                }
        }
        if (ok) {
            ok = constraints(v, out, c, path);
            CheckCtx check{&c.errors, path, c.options.maxErrors};
            for (const auto& f : checks_) f(out, check);
        }
        return ok;
    }
    Result<T> decode(const json::Value& v, const DecodeOptions& options = {},
                     std::vector<Warning>* warnings = nullptr) const {
        DecodeCtx c{options, {}, warnings};
        T out{};
        decodeInto(v, out, c, "");
        if (c.errors.size() >= options.maxErrors)
            c.errors.emplace_back("schema.too_many_errors", "too many validation errors");
        if (c.errors.empty()) return out;
        if (c.errors.size() == 1) return c.errors.front();
        Error e("schema.invalid", std::to_string(c.errors.size()) + " problems in " + title_);
        e.details = std::move(c.errors);
        return e;
    }
    json::Value encode(const T& v) const { return encoder_(v); }
    Status validate(const json::Value& v, const DecodeOptions& o = {}) const {
        auto r = decode(v, o);
        return r ? Status{} : Status{r.error()};
    }
    json::Value jsonSchema() const {
        if (!unit_) return export_;
        json::Value result = json::Value::object();
        json::Value numeric = json::Value::object();
        numeric["type"] = "number";
        for (const auto& member : export_.asObject()) {
            if (member.key == "minimum" || member.key == "maximum" ||
                member.key == "exclusiveMinimum" || member.key == "exclusiveMaximum")
                numeric[member.key] = member.value;
        }
        json::Value quantity = json::Value::object();
        quantity["type"] = "string";
        quantity["pattern"] = quantityPattern(*unit_);
        result["anyOf"] = json::Array{std::move(numeric), std::move(quantity)};
        for (const auto& member : export_.asObject()) {
            if (member.key == "type" || member.key == "minimum" || member.key == "maximum" ||
                member.key == "exclusiveMinimum" || member.key == "exclusiveMaximum")
                continue;
            result[member.key] = member.value;
        }
        const auto* description = result.find("description");
        result["description"] = (description ? description->asString() + " " : std::string{}) +
                                "Quantity strings are range-checked by the server.";
        return result;
    }
    Schema doc(std::string text) const {
        auto s = *this;
        s.export_["description"] = std::move(text);
        return s;
    }
    Schema example(T value) const {
        auto s = *this;
        s.example_ = s.encode(value);
        s.export_["examples"] = json::Array{s.example_.value()};
        return s;
    }
    Schema deprecated(std::string text) const {
        auto s = *this;
        s.export_["deprecated"] = true;
        s.export_["x-deprecated-message"] = std::move(text);
        return s;
    }
    Schema min(double v) const {
        auto s = *this;
        s.minimum_ = v;
        s.export_["minimum"] = v;
        return s;
    }
    Schema max(double v) const {
        auto s = *this;
        s.maximum_ = v;
        s.export_["maximum"] = v;
        return s;
    }
    Schema range(double lo, double hi) const { return min(lo).max(hi); }
    Schema exclusiveMin(double v) const {
        auto s = *this;
        s.exclusiveMinimum_ = v;
        s.export_["exclusiveMinimum"] = v;
        return s;
    }
    Schema exclusiveMax(double v) const {
        auto s = *this;
        s.exclusiveMaximum_ = v;
        s.export_["exclusiveMaximum"] = v;
        return s;
    }
    Schema unit(Unit u) const {
        auto s = *this;
        s.unit_ = u;
        s.export_["x-unit"] = std::string(unitName(u));
        s.export_["x-unit-strings"] = true;
        s.export_["type"] = json::Array{std::string("number"), std::string("string")};
        return s;
    }
    Schema minLength(std::size_t n) const {
        auto s = *this;
        s.minLength_ = n;
        s.export_["minLength"] = static_cast<unsigned long long>(n);
        return s;
    }
    Schema maxLength(std::size_t n) const {
        auto s = *this;
        s.maxLength_ = n;
        s.export_["maxLength"] = static_cast<unsigned long long>(n);
        return s;
    }
    Schema pattern(std::string p, std::string label) const {
        auto s = *this;
        s.pattern_ = std::move(p);
        s.patternLabel_ = std::move(label);
        s.export_["pattern"] = *s.pattern_;
        return s;
    }
    Schema format(std::string f) const {
        auto s = *this;
        s.export_["format"] = std::move(f);
        return s;
    }
    Schema minItems(std::size_t n) const {
        auto s = *this;
        s.minItems_ = n;
        s.export_["minItems"] = static_cast<unsigned long long>(n);
        return s;
    }
    Schema maxItems(std::size_t n) const {
        auto s = *this;
        s.maxItems_ = n;
        s.export_["maxItems"] = static_cast<unsigned long long>(n);
        return s;
    }
    Schema uniqueItems() const {
        auto s = *this;
        s.unique_ = true;
        s.export_["uniqueItems"] = true;
        return s;
    }
    Schema keyPattern(std::string p, std::string label) const {
        auto s = *this;
        s.keyPattern_ = std::move(p);
        s.keyLabel_ = std::move(label);
        s.export_["propertyNames"]["pattern"] = *s.keyPattern_;
        return s;
    }
    Schema check(std::function<void(const T&, CheckCtx&)> f) const {
        auto s = *this;
        s.checks_.push_back(std::move(f));
        return s;
    }
    const std::string& title() const { return title_; }
    const std::optional<json::Value>& exampleValue() const { return example_; }

  private:
    std::string title_;
    Decoder decoder_;
    Encoder encoder_;
    json::Value export_;
    std::optional<json::Value> example_;
    std::optional<double> minimum_, maximum_, exclusiveMinimum_, exclusiveMaximum_;
    std::optional<Unit> unit_;
    std::optional<std::size_t> minLength_, maxLength_, minItems_, maxItems_;
    bool unique_ = false;
    std::optional<std::string> pattern_, patternLabel_, keyPattern_, keyLabel_;
    std::vector<std::function<void(const T&, CheckCtx&)>> checks_;

    // Compare integral values without rounding them to double. The power-of-two
    // limits guard the cast, including the unrepresentable positive int64 limit.
    static std::partial_ordering compareBound(T value, double bound) {
        if constexpr (std::is_integral_v<T>) {
            if (std::isnan(bound)) return std::partial_ordering::unordered;
            const double upper = std::ldexp(1.0, std::numeric_limits<T>::digits);
            const double lower = std::is_signed_v<T> ? -upper : 0.0;
            if (bound >= upper) return std::partial_ordering::less;
            if (bound < lower) return std::partial_ordering::greater;
            const T whole = static_cast<T>(bound);
            if (value < whole) return std::partial_ordering::less;
            if (value > whole) return std::partial_ordering::greater;
            return std::trunc(bound) <=> bound;
        } else {
            using Common = std::common_type_t<T, double>;
            return static_cast<Common>(value) <=> static_cast<Common>(bound);
        }
    }

  public:
    bool constraints(const json::Value& v, T& out, DecodeCtx& c, const std::string& path) const {
        if constexpr (std::is_arithmetic_v<T> && !std::is_same_v<T, bool>) {
            if constexpr (std::is_floating_point_v<T>) {
                if (!std::isfinite(out)) {
                    c.error("schema.not_finite", "number must be finite", path);
                    return false;
                }
            }
            if ((minimum_ && compareBound(out, *minimum_) < 0) ||
                (maximum_ && compareBound(out, *maximum_) > 0) ||
                (exclusiveMinimum_ && compareBound(out, *exclusiveMinimum_) <= 0) ||
                (exclusiveMaximum_ && compareBound(out, *exclusiveMaximum_) >= 0)) {
                std::string hint = "valid range: " +
                                   (minimum_            ? std::to_string(*minimum_)
                                    : exclusiveMinimum_ ? std::to_string(*exclusiveMinimum_)
                                                        : "-infinity") +
                                   " .. " +
                                   (maximum_            ? std::to_string(*maximum_)
                                    : exclusiveMaximum_ ? std::to_string(*exclusiveMaximum_)
                                                        : "infinity");
                if (unit_) hint += " " + std::string(unitName(*unit_));
                c.error("schema.out_of_range", "number outside valid range", path, hint);
                return false;
            }
        }
        if constexpr (std::is_same_v<T, std::string>) {
            // In characters (code points), as JSON Schema's minLength/maxLength.
            const auto length =
                static_cast<std::size_t>(std::count_if(out.begin(), out.end(), [](char ch) {
                    return (static_cast<unsigned char>(ch) & 0xc0) != 0x80;
                }));
            if ((minLength_ && length < *minLength_) || (maxLength_ && length > *maxLength_)) {
                c.error("schema.length", "string length is outside limits", path,
                        "length must be " + std::to_string(minLength_.value_or(0)) + " .. " +
                            std::to_string(
                                maxLength_.value_or(std::numeric_limits<std::size_t>::max())) +
                            " characters");
                return false;
            }
            if (pattern_ && !std::regex_match(out, std::regex(*pattern_))) {
                c.error("schema.pattern", "string does not match pattern", path,
                        "use " + patternLabel_.value_or(*pattern_));
                return false;
            }
        }
        if constexpr (requires(T t) {
                          t.size();
                          typename T::value_type;
                      } && !std::is_same_v<T, std::string>) {
            if ((minItems_ && out.size() < *minItems_) || (maxItems_ && out.size() > *maxItems_)) {
                c.error("schema.items", "container size is outside limits", path,
                        "item count must be " + std::to_string(minItems_.value_or(0)) + " .. " +
                            std::to_string(
                                maxItems_.value_or(std::numeric_limits<std::size_t>::max())));
                return false;
            }
            if constexpr (requires(T t) { t[std::size_t{}]; })
                if (unique_) {
                    auto encoded = encoder_(out);
                    for (std::size_t i = 0; i < encoded.size(); ++i)
                        for (std::size_t j = 0; j < i; ++j)
                            if (encoded.at(i) == encoded.at(j)) {
                                c.error("schema.unique", "duplicate array item",
                                        json::pointerJoin(path, i),
                                        "first duplicate at index " + std::to_string(j));
                                return false;
                            }
                }
        }
        if constexpr (requires(T t) {
                          typename T::mapped_type;
                          requires std::is_convertible_v<typename T::key_type, std::string>;
                      })
            if (keyPattern_)
                for (const auto& [key, item] : out) {
                    (void)item;
                    if (!std::regex_match(key, std::regex(*keyPattern_))) {
                        c.error("schema.key", "map key does not match pattern",
                                json::pointerJoin(path, key), "use " + keyLabel());
                        return false;
                    }
                }
        (void)v;
        return true;
    }
    std::optional<Unit> declaredUnit() const { return unit_; }
    bool unique() const { return unique_; }
    std::optional<std::string> keyRegex() const { return keyPattern_; }
    std::string keyLabel() const { return keyLabel_.value_or("valid key"); }
};
inline json::Value typeExport(std::string_view type) {
    json::Value v = json::Value::object();
    v["type"] = std::string(type);
    return v;
}
inline void wrongType(DecodeCtx& c, const std::string& path, std::string expected,
                      const std::optional<json::Value>& example = {}) {
    c.error("schema.type", "expected " + expected, path,
            "expected " + expected +
                (example ? "; example: " + json::writeCompact(*example)
                         : "; use a valid " + expected + " value"));
}
inline Schema<bool> boolean() {
    return {"boolean",
            [](const json::Value& v, bool& out, DecodeCtx& c, const std::string& p) {
                if (!v.isBool()) {
                    wrongType(c, p, "boolean");
                    return false;
                }
                out = v.asBool();
                return true;
            },
            [](bool v) { return json::Value(v); }, typeExport("boolean")};
}
inline Schema<std::string> string() {
    return {"string",
            [](const json::Value& v, std::string& out, DecodeCtx& c, const std::string& p) {
                if (!v.isString()) {
                    wrongType(c, p, "string");
                    return false;
                }
                out = v.asString();
                return true;
            },
            [](const std::string& v) { return json::Value(v); }, typeExport("string")};
}
inline Schema<json::Value> any() {
    return {"value",
            [](const json::Value& v, json::Value& out, DecodeCtx&, const std::string&) {
                out = v;
                return true;
            },
            [](const json::Value& v) { return v; }, json::Value::object()};
}
inline Schema<double> number() {
    return {"number",
            [](const json::Value& v, double& out, DecodeCtx& c, const std::string& p) {
                if (v.isNumber()) {
                    out = v.asDouble();
                    return true;
                }
                wrongType(c, p, "number");
                return false;
            },
            [](double v) { return json::Value(v); }, typeExport("number")};
}
inline Schema<std::int64_t> integer() {
    return {"integer",
            [](const json::Value& v, std::int64_t& out, DecodeCtx& c, const std::string& p) {
                if (v.isInt()) {
                    out = v.asInt();
                    return true;
                }
                if (v.isDouble() && c.options.coerceIntegralDoubles &&
                    std::isfinite(v.asDouble()) && std::trunc(v.asDouble()) == v.asDouble() &&
                    v.asDouble() >= -0x1p63 && v.asDouble() < 0x1p63) {
                    out = static_cast<std::int64_t>(v.asDouble());
                    return true;
                }
                if (v.isNumber())
                    c.error("schema.integer_required", "expected an integer", p,
                            "nearest integer: " + std::to_string(std::round(v.asDouble())));
                else
                    wrongType(c, p, "integer");
                return false;
            },
            [](std::int64_t v) { return json::Value(static_cast<long long>(v)); },
            typeExport("integer")};
}
template <class I>
Schema<I> integerOf() {
    auto base = integer();
    return {"integer",
            [base](const json::Value& v, I& out, DecodeCtx& c, const std::string& p) {
                std::int64_t n = 0;
                if (!base.decodeInto(v, n, c, p)) return false;
                if constexpr (std::is_unsigned_v<I>) {
                    if (n < 0 || static_cast<std::uint64_t>(n) > std::numeric_limits<I>::max()) {
                        c.error(
                            "schema.out_of_range", "integer outside target range", p,
                            "valid range: 0 .. " + std::to_string(std::numeric_limits<I>::max()));
                        return false;
                    }
                } else if (n < std::numeric_limits<I>::min() || n > std::numeric_limits<I>::max()) {
                    c.error("schema.out_of_range", "integer outside target range", p,
                            "valid range: " + std::to_string(std::numeric_limits<I>::min()) +
                                " .. " + std::to_string(std::numeric_limits<I>::max()));
                    return false;
                }
                out = static_cast<I>(n);
                return true;
            },
            [](I v) { return json::Value(static_cast<long long>(v)); }, typeExport("integer")};
}
template <class E>
Schema<E> enumeration(std::initializer_list<std::pair<E, std::string_view>> choices) {
    std::vector<std::pair<E, std::string>> list;
    json::Array names;
    for (auto [e, s] : choices) {
        list.emplace_back(e, s);
        names.emplace_back(std::string(s));
    }
    auto ex = typeExport("string");
    ex["enum"] = std::move(names);
    return {"enum",
            [list](const json::Value& v, E& out, DecodeCtx& c, const std::string& p) {
                if (!v.isString()) {
                    wrongType(c, p, "string enum");
                    return false;
                }
                for (const auto& [e, s] : list)
                    if (s == v.asString()) {
                        out = e;
                        return true;
                    }
                std::vector<std::string_view> opts;
                std::string all;
                for (const auto& [e, s] : list) {
                    (void)e;
                    opts.push_back(s);
                }
                all = listNames(opts);
                auto match = closestMatch(v.asString(), opts);
                c.error(
                    "schema.enum", "unknown enum choice", p,
                    (match.empty() ? "" : "did you mean \"" + match + "\"? ") + "choices: " + all);
                return false;
            },
            [list](E v) {
                for (const auto& [e, s] : list)
                    if (e == v) return json::Value(s);
                return json::Value();
            },
            ex};
}
template <class T>
Schema<std::optional<T>> optional(Schema<T> inner) {
    auto ex = inner.jsonSchema();
    auto in = std::make_shared<const Schema<T>>(std::move(inner));  // copies share it
    return {"optional",
            [in](const json::Value& v, std::optional<T>& out, DecodeCtx& c, const std::string& p) {
                if (v.isNull()) {
                    out.reset();
                    return true;
                }
                T tmp{};
                bool ok = in->decodeInto(v, tmp, c, p);
                if (ok) out = std::move(tmp);
                return ok;
            },
            [in](const std::optional<T>& v) { return v ? in->encode(*v) : json::Value(); }, ex};
}
template <class T>
Schema<std::vector<T>> arrayOf(Schema<T> item) {
    auto ex = typeExport("array");
    ex["items"] = item.jsonSchema();
    auto it = std::make_shared<const Schema<T>>(std::move(item));
    return {"array",
            [it](const json::Value& v, std::vector<T>& out, DecodeCtx& c, const std::string& p) {
                if (!v.isArray()) {
                    wrongType(c, p, "array");
                    return false;
                }
                out.clear();
                bool ok = true;
                for (std::size_t i = 0; i < v.size(); ++i) {
                    T x{};
                    if (it->decodeInto(v.at(i), x, c, json::pointerJoin(p, i)))
                        out.push_back(std::move(x));
                    else
                        ok = false;
                }
                return ok;
            },
            [it](const std::vector<T>& v) {
                json::Value out = json::Value::array();
                for (const auto& x : v) out.push_back(it->encode(x));
                return out;
            },
            ex};
}
template <class T>
Schema<std::map<std::string, T>> mapOf(Schema<T> value) {
    auto ex = typeExport("object");
    ex["additionalProperties"] = value.jsonSchema();
    auto val = std::make_shared<const Schema<T>>(std::move(value));
    return {"map",
            [val](const json::Value& v, std::map<std::string, T>& out, DecodeCtx& c,
                  const std::string& p) {
                if (!v.isObject()) {
                    wrongType(c, p, "object");
                    return false;
                }
                bool ok = true;
                for (const auto& m : v.asObject()) {
                    T x{};
                    if (val->decodeInto(m.value, x, c, json::pointerJoin(p, m.key)))
                        out[m.key] = std::move(x);
                    else
                        ok = false;
                }
                return ok;
            },
            [val](const std::map<std::string, T>& v) {
                json::Value out = json::Value::object();
                for (const auto& [k, x] : v) out[k] = val->encode(x);
                return out;
            },
            ex};
}

template <class T>
class ObjectBuilder {
    struct Field {
        std::string name, doc, expected;
        bool optional = false, omitDefault = false, hasDefault = false;
        json::Value schema, defaultJson;
        std::function<void(const json::Value*, T&, DecodeCtx&, const std::string&)> decode;
        std::function<std::optional<json::Value>(const T&)> encode;
        std::function<void(T&)> applyDefault;
        std::function<void(T&, const std::any&)> setDefault;
        std::function<json::Value(const std::any&)> encodeDefault;
    };

  public:
    explicit ObjectBuilder(std::string title) : title_(std::move(title)) {}
    ObjectBuilder& doc(std::string text) {
        doc_ = std::move(text);
        return *this;
    }
    ObjectBuilder& unknownMembers(UnknownMembers p) {
        policy_ = p;
        return *this;
    }
    // A member that belongs to another command: the unknown-member error says where it is set
    // (`elsewhere("tempo", "tempo.set sets the tempo")`).
    ObjectBuilder& elsewhere(std::string member, std::string hint) {
        elsewhere_.emplace_back(std::move(member), std::move(hint));
        return *this;
    }
    template <class M>
    ObjectBuilder& field(std::string name, M T::* member, Schema<M> schema) {
        Field f;
        f.name = std::move(name);
        f.optional = IsOptional<M>::value;
        f.schema = schema.jsonSchema();
        f.expected = schema.title();
        if (auto* d = f.schema.find("description")) f.doc = d->asString();
        auto fieldName = f.name;
        auto fieldDoc = f.doc;
        // The three closures share one copy of the member schema.
        auto shared = std::make_shared<const Schema<M>>(std::move(schema));
        f.decode = [member, shared, fieldName, fieldDoc](const json::Value* v, T& obj, DecodeCtx& c,
                                                         const std::string& p) {
            if (!v) {
                if constexpr (!IsOptional<M>::value)
                    c.error("schema.required", "missing required member \"" + fieldName + "\"", p,
                            "add " + fieldName + " (" + shared->title() + ")" +
                                (fieldDoc.empty() ? "" : ": " + fieldDoc));
                return;
            }
            shared->decodeInto(*v, obj.*member, c, p);
        };
        f.encode = [member, shared](const T& obj) -> std::optional<json::Value> {
            if constexpr (IsOptional<M>::value) {
                if (!(obj.*member)) return std::nullopt;
            }
            return shared->encode(obj.*member);
        };
        f.applyDefault = [](T&) {};
        f.setDefault = [member](T& obj, const std::any& v) { obj.*member = std::any_cast<M>(v); };
        f.encodeDefault = [shared](const std::any& v) {
            return shared->encode(std::any_cast<M>(v));
        };
        fields_.push_back(std::move(f));
        return *this;
    }
    template <class M>
    ObjectBuilder& field(std::string name, std::optional<M> T::* member, Schema<M> schema) {
        return field(std::move(name), member, optional(std::move(schema)));
    }
    ObjectBuilder& field(std::string name, float T::* member, Schema<double> schema) {
        auto shared = std::make_shared<const Schema<double>>(schema);
        Schema<float> floatSchema(
            schema.title(),
            [shared](const json::Value& value, float& out, DecodeCtx& ctx,
                     const std::string& path) {
                double decoded = 0;
                if (!shared->decodeInto(value, decoded, ctx, path)) return false;
                if (decoded < -static_cast<double>(std::numeric_limits<float>::max()) ||
                    decoded > static_cast<double>(std::numeric_limits<float>::max())) {
                    ctx.error("schema.out_of_range", "number outside float range", path,
                              "use a finite float value");
                    return false;
                }
                out = static_cast<float>(decoded);
                return true;
            },
            [shared](float value) { return shared->encode(static_cast<double>(value)); },
            schema.jsonSchema());
        return field(std::move(name), member, std::move(floatSchema));
    }
    template <class M>
    ObjectBuilder& defaultValue(M value) {
        auto& f = fields_.back();
        f.hasDefault = true;
        if constexpr (std::is_constructible_v<json::Value, M>)
            f.defaultJson = json::Value(std::move(value));
        else
            f.defaultJson = f.encodeDefault(std::any(std::move(value)));
        f.applyDefault = [encoded = f.defaultJson, decoder = f.decode](T& obj) {
            DecodeCtx ctx;
            decoder(&encoded, obj, ctx, "");
        };
        return *this;
    }
    ObjectBuilder& omitIfDefault() {
        fields_.back().omitDefault = true;
        return *this;
    }
    ObjectBuilder& check(std::function<void(const T&, CheckCtx&)> f) {
        checks_.push_back(std::move(f));
        return *this;
    }
    Schema<T> build() const {
        json::Value ex = typeExport("object");
        ex["title"] = title_;
        if (!doc_.empty()) ex["description"] = doc_;
        json::Value props = json::Value::object(), required = json::Value::array();
        for (const auto& f : fields_) {
            props[f.name] = f.schema;
            if (!f.optional && !f.hasDefault) required.push_back(f.name);
            if (f.hasDefault) props[f.name]["default"] = f.defaultJson;
        }
        ex["properties"] = std::move(props);
        if (required.size()) ex["required"] = std::move(required);
        ex["additionalProperties"] =
            policy_.value_or(UnknownMembers::Error) == UnknownMembers::Error ? false : true;
        auto fields =
            std::make_shared<const std::vector<Field>>(fields_);  // shared by both closures
        auto policy = policy_;
        auto title = title_;
        auto elsewhere = elsewhere_;
        auto decoder = [fields, policy, elsewhere](const json::Value& v, T& out, DecodeCtx& c,
                                                   const std::string& p) {
            if (!v.isObject()) {
                wrongType(c, p, "object");
                return false;
            }
            auto before = c.errors.size();
            for (const auto& f : *fields) {
                auto* member = v.find(f.name);
                auto path = json::pointerJoin(p, f.name);
                if (!member && f.hasDefault)
                    f.applyDefault(out);
                else {
                    auto previous = c.errors.size();
                    f.decode(member, out, c, path);
                    if (f.hasDefault && !f.schema.find("examples"))
                        for (std::size_t i = previous; i < c.errors.size(); ++i)
                            if (c.errors[i].code == "schema.type")
                                c.errors[i].hint = "expected " + f.expected + "; example: " +
                                                   json::writeCompact(f.defaultJson);
                }
            }
            for (const auto& m : v.asObject()) {
                bool known = false;
                for (const auto& f : *fields)
                    if (m.key == f.name) {
                        known = true;
                        break;
                    }
                if (known) continue;
                auto path = json::pointerJoin(p, m.key);
                if (policy.value_or(c.options.unknownMembers) == UnknownMembers::Ignore) {
                    if (c.warnings)
                        c.warnings->push_back(
                            {"schema.unknown_member", "ignored unknown member", path});
                    continue;
                }
                std::vector<std::string_view> names;
                for (const auto& f : *fields) {
                    names.push_back(f.name);
                }
                std::string hint = listNames(names);
                auto match = closestMatch(m.key, names);
                if (!match.empty())
                    hint = "did you mean \"" + match + "\"? valid members: " + hint;
                else
                    hint = "valid members: " + hint;
                for (const auto& [member, where] : elsewhere)
                    if (member == m.key) hint = where + "; " + hint;
                c.error("schema.unknown_member", "unknown member \"" + m.key + "\"", path, hint);
            }
            return c.errors.size() == before;
        };
        auto encoder = [fields](const T& obj) {
            json::Value v = json::Value::object();
            for (const auto& f : *fields) {
                auto x = f.encode(obj);
                if (x && !(f.omitDefault && f.hasDefault && *x == f.defaultJson))
                    v[f.name] = std::move(*x);
            }
            return v;
        };
        Schema<T> result(title, decoder, encoder, ex);
        for (const auto& check : checks_) result = result.check(check);
        return result;
    }

  private:
    std::string title_, doc_;
    std::optional<UnknownMembers> policy_;
    std::vector<std::pair<std::string, std::string>> elsewhere_;
    std::vector<Field> fields_;
    std::vector<std::function<void(const T&, CheckCtx&)>> checks_;
};
template <class T>
ObjectBuilder<T> object(std::string_view title) {
    return ObjectBuilder<T>(std::string(title));
}

template <class V>
class VariantBuilder {
    struct Alternative {
        std::string tag;
        std::function<bool(const json::Value&, V&, DecodeCtx&, const std::string&)> decode;
        std::function<std::optional<json::Value>(const V&)> encode;
        json::Value schema;
    };

  public:
    VariantBuilder(std::string title, std::string tag)
        : title_(std::move(title)), tag_(std::move(tag)) {}
    template <class A>
    VariantBuilder& alternative(std::string tag, Schema<A> schema) {
        Alternative alt;
        alt.tag = std::move(tag);
        alt.schema = schema.jsonSchema();
        auto shared = std::make_shared<const Schema<A>>(std::move(schema));
        alt.decode = [shared](const json::Value& v, V& out, DecodeCtx& c, const std::string& p) {
            A x{};
            bool ok = shared->decodeInto(v, x, c, p);
            if (ok) out = std::move(x);
            return ok;
        };
        alt.encode = [shared](const V& v) -> std::optional<json::Value> {
            if (!std::holds_alternative<A>(v)) return std::nullopt;
            return shared->encode(std::get<A>(v));
        };
        alts_.push_back(std::move(alt));
        return *this;
    }
    Schema<V> build() const {
        json::Value ex = json::Value::object();
        ex["title"] = title_;
        ex["discriminator"]["propertyName"] = tag_;
        json::Value one = json::Value::array();
        for (const auto& a : alts_) {
            auto s = a.schema;
            s["properties"][tag_]["const"] = a.tag;
            s["required"].push_back(tag_);
            one.push_back(std::move(s));
        }
        ex["oneOf"] = std::move(one);
        auto alts = std::make_shared<const std::vector<Alternative>>(alts_);
        auto tag = tag_;
        auto decoder = [alts, tag](const json::Value& v, V& out, DecodeCtx& c,
                                   const std::string& p) {
            if (!v.isObject()) {
                wrongType(c, p, "object");
                return false;
            }
            const auto* t = v.find(tag);
            std::vector<std::string_view> names;
            for (const auto& a : *alts) {
                names.push_back(a.tag);
            }
            const auto all = listNames(names);
            if (!t || !t->isString()) {
                c.error("schema.missing_tag", "missing variant tag", json::pointerJoin(p, tag),
                        "add \"" + tag + "\" with one of: " + all);
                return false;
            }
            for (const auto& a : *alts)
                if (a.tag == t->asString()) {
                    json::Value without = v;
                    without.asObject().erase(tag);
                    return a.decode(without, out, c, p);
                }
            auto match = closestMatch(t->asString(), names);
            c.error(
                "schema.unknown_variant", "unknown variant tag", json::pointerJoin(p, tag),
                (match.empty() ? "" : "did you mean \"" + match + "\"? ") + "valid tags: " + all);
            return false;
        };
        auto encoder = [alts, tag](const V& v) {
            json::Value out = json::Value::object();
            for (const auto& a : *alts) {
                auto x = a.encode(v);
                if (x) {
                    out[tag] = a.tag;
                    if (x->isObject())
                        for (const auto& m : x->asObject()) out[m.key] = m.value;
                    break;
                }
            }
            return out;
        };
        return Schema<V>(title_, decoder, encoder, ex);
    }

  private:
    std::string title_, tag_;
    std::vector<Alternative> alts_;
};
template <class V>
VariantBuilder<V> variant(std::string_view title, std::string_view tag = "type") {
    return VariantBuilder<V>(std::string(title), std::string(tag));
}
template <class T>
Schema<T> named(std::string_view name, Schema<T> s) {
    auto ex = json::Value::object();
    ex["$ref"] = "#/$defs/" + std::string(name);
    ex["$defs"][std::string(name)] = s.jsonSchema();
    const auto title = s.title();
    auto shared = std::make_shared<const Schema<T>>(std::move(s));
    return Schema<T>(
        title,
        [shared](const json::Value& v, T& out, DecodeCtx& c, const std::string& p) {
            return shared->decodeInto(v, out, c, p);
        },
        [shared](const T& v) { return shared->encode(v); }, ex);
}
}  // namespace supple::schema
