// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <supple/schema/document.h>
#include <supple/schema/schema.h>
#include <filesystem>
#include <fstream>
#include <limits>
using namespace supple;
using namespace supple::schema;
struct Filter {
    double cutoff = 0;
    std::string mode;
    std::optional<double> drive;
};
TEST_CASE("optional null is omitted while its export deliberately stays narrower") {
    auto s = object<Filter>("optional_filter")
                 .field("drive", &Filter::drive, number().range(0, 24))
                 .build();
    const auto omitted = s.decode(json::Value::object());
    const auto explicitNull = s.decode(*json::parse(R"({"drive":null})"));
    REQUIRE(omitted);
    REQUIRE(explicitNull);
    CHECK_FALSE(explicitNull->drive);
    CHECK(s.encode(*omitted) == s.encode(*explicitNull));
    const auto exported = s.jsonSchema();
    const auto* field = exported.find("properties")->find("drive");
    REQUIRE(field);
    CHECK(*field->find("type") == "number");
    CHECK_FALSE(field->find("anyOf"));
    CHECK_FALSE(s.decode(*json::parse(R"({"drive":false})")));
    CHECK_FALSE(s.decode(*json::parse(R"({"drive":25})")));
}
TEST_CASE("object decode encode and errors") {
    auto s = object<Filter>("filter")
                 .field("cutoff", &Filter::cutoff, number().range(20, 20000).unit(Unit::Hz))
                 .defaultValue(1000.0)
                 .field("mode", &Filter::mode, string())
                 .field("drive", &Filter::drive, optional(number().unit(Unit::dB)))
                 .build();
    auto v = json::parse("{\"mode\":\"lowpass\",\"drive\":\"3 dB\"}");
    REQUIRE(v);
    auto r = s.decode(*v);
    REQUIRE(r);
    CHECK(r->cutoff == 1000);
    CHECK(r->drive == 3);
    auto encoded = s.encode(*r);
    CHECK(encoded.find("cutoff")->asDouble() == 1000);
    auto bad = json::parse("{\"cuttoff\":5}");
    REQUIRE(bad);
    auto error = s.decode(*bad);
    REQUIRE_FALSE(error);
    CHECK(error.error().code == "schema.invalid");
    CHECK(error.error().details.size() == 2);
}
TEST_CASE("units") {
    auto n = parseQuantity("1.2 kHz", Unit::Hz);
    REQUIRE(n);
    CHECK(*n == 1200);
    CHECK(parseQuantity("0.25 s", Unit::ms).value() == 250);
    CHECK_FALSE(parseQuantity("5 ms", Unit::Hz));
}
TEST_CASE("enum, arrays, maps and variant") {
    enum class Wave { sine, saw };
    struct Osc {
        Wave wave = Wave::sine;
    };
    struct Noise {
        int seed = 0;
    };
    using Module = std::variant<Osc, Noise>;
    auto wave = enumeration<Wave>({{Wave::sine, "sine"}, {Wave::saw, "saw"}});
    auto osc = object<Osc>("osc").field("wave", &Osc::wave, wave).build();
    auto noise = object<Noise>("noise").field("seed", &Noise::seed, integerOf<int>()).build();
    auto modules = variant<Module>("module")
                       .alternative<Osc>("osc", osc)
                       .alternative<Noise>("noise", noise)
                       .build();
    auto list = arrayOf(modules).minItems(1);
    auto v = json::parse("[{\"type\":\"osc\",\"wave\":\"saw\"},{\"type\":\"noise\",\"seed\":7}]");
    REQUIRE(v);
    auto r = list.decode(*v);
    REQUIRE(r);
    CHECK(std::get<Osc>(r->at(0)).wave == Wave::saw);
    CHECK(json::writeCompact(list.encode(*r)) == json::writeCompact(*v));
    auto bad = json::parse("[{\"type\":\"oc\",\"wave\":\"saw\"}]");
    REQUIRE(bad);
    auto e = list.decode(*bad);
    REQUIRE_FALSE(e);
    CHECK(e.error().code == "schema.unknown_variant");
    CHECK(e.error().hint.find("osc") != std::string::npos);
    auto m = mapOf(integerOf<int>()).keyPattern("^[a-z]+$", "lowercase key");
    auto input = json::parse("{\"Bad\":1}");
    REQUIRE(input);
    auto mr = m.decode(*input);
    REQUIRE_FALSE(mr);
    CHECK(mr.error().code == "schema.key");
}
TEST_CASE("schema constraints and warnings") {
    auto limited = number().range(20, 20000).unit(Unit::Hz);
    auto high = json::Value("21 kHz");
    auto e = limited.decode(high);
    REQUIRE_FALSE(e);
    CHECK(e.error().code == "schema.out_of_range");
    CHECK(e.error().hint.find("Hz") != std::string::npos);
    auto str = string().minLength(2).maxLength(5).pattern("^[a-z]+$", "lowercase letters");
    auto s = str.decode(json::Value("A"));
    REQUIRE_FALSE(s);
    CHECK(s.error().code == "schema.length");
    s = str.decode(json::Value("AA"));
    REQUIRE_FALSE(s);
    CHECK(s.error().code == "schema.pattern");
    auto arr = arrayOf(integerOf<int>()).uniqueItems();
    auto d = json::parse("[1,2,1]");
    REQUIRE(d);
    auto duplicate = arr.decode(*d);
    REQUIRE_FALSE(duplicate);
    CHECK(duplicate.error().code == "schema.unique");
    CHECK(duplicate.error().hint.find("0") != std::string::npos);
    struct Config {
        int value = 0;
    };
    auto schema = object<Config>("config").field("value", &Config::value, integerOf<int>()).build();
    auto v = json::parse("{\"value\":1,\"valu\":2}");
    REQUIRE(v);
    std::vector<Warning> warnings;
    DecodeOptions options;
    options.unknownMembers = UnknownMembers::Ignore;
    auto decoded = schema.decode(*v, options, &warnings);
    REQUIRE(decoded);
    CHECK(warnings.size() == 1);
    CHECK(warnings.front().path == "/valu");
    auto invalid = schema.decode(*v);
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().code == "schema.unknown_member");
    CHECK(invalid.error().hint.find("value") != std::string::npos);
}
TEST_CASE("an unknown member that belongs elsewhere says where") {
    struct Config {
        int value = 0;
    };
    auto schema = object<Config>("config")
                      .elsewhere("rate", "the rate is set with rate.set")
                      .field("value", &Config::value, integerOf<int>())
                      .build();
    auto moved = schema.decode(*json::parse("{\"value\":1,\"rate\":1}"));
    REQUIRE_FALSE(moved);
    CHECK(moved.error().code == "schema.unknown_member");
    CHECK(moved.error().hint == "the rate is set with rate.set; valid members: value");
    auto other = schema.decode(*json::parse("{\"value\":1,\"speed\":1}"));
    REQUIRE_FALSE(other);
    CHECK(other.error().hint == "valid members: value");
}
TEST_CASE("unit aliases") {
    struct Alias {
        const char* text;
        Unit unit;
        double value;
    };
    const Alias aliases[] = {
        {"1.2k", Unit::Hz, 1200},      {"2 khz", Unit::Hz, 2000},   {"0.25 s", Unit::ms, 250},
        {"1000 us", Unit::ms, 1},      {"1000 µs", Unit::ms, 1},    {"-3db", Unit::dB, -3},
        {"-14 LKFS", Unit::LUFS, -14}, {"3 LU", Unit::LU, 3},       {"7 semi", Unit::st, 7},
        {"50 cents", Unit::ct, 50},    {"2 octaves", Unit::oct, 2}, {"50%", Unit::percent, 50},
        {"4:1", Unit::ratio, 4},       {"2×", Unit::x, 2},          {"90°", Unit::deg, 90},
        {"120 bpm", Unit::bpm, 120},   {"4 qn", Unit::beats, 4},    {"512 smp", Unit::samples, 512},
        {"12.5 m", Unit::m, 12.5},     {"250 cm", Unit::m, 2.5},    {"3", Unit::none, 3}};
    for (const auto& a : aliases) {
        auto r = parseQuantity(a.text, a.unit);
        INFO(a.text);
        REQUIRE(r);
        CHECK(*r == a.value);
    }
}
TEST_CASE("schema error codes") {
    auto type = number().decode(json::Value(true));
    REQUIRE_FALSE(type);
    CHECK(type.error().code == "schema.type");
    auto nonfinite = number().decode(json::Value(std::numeric_limits<double>::infinity()));
    REQUIRE_FALSE(nonfinite);
    CHECK(nonfinite.error().code == "schema.not_finite");
    auto integral = integer().decode(json::Value(1.5));
    REQUIRE_FALSE(integral);
    CHECK(integral.error().code == "schema.integer_required");
    CHECK(integral.error().hint.find("nearest integer") != std::string::npos);
    auto unit = number().unit(Unit::Hz).decode(json::Value("2 ms"));
    REQUIRE_FALSE(unit);
    CHECK(unit.error().code == "schema.unit");
    auto empty = arrayOf(integer()).minItems(1).decode(json::Value::array());
    REQUIRE_FALSE(empty);
    CHECK(empty.error().code == "schema.items");
    auto wave = enumeration<int>({{0, "sine"}, {1, "saw"}}).decode(json::Value("sow"));
    REQUIRE_FALSE(wave);
    CHECK(wave.error().code == "schema.enum");
    CHECK(wave.error().hint.find("saw") != std::string::npos);
    struct A {
        int value = 0;
    };
    using V = std::variant<A, std::string>;
    auto a = object<A>("a").field("value", &A::value, integerOf<int>()).build();
    auto v = variant<V>("variant").alternative<A>("a", a).build();
    auto missing = v.decode(json::Value::object());
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == "schema.missing_tag");
    auto capped = json::parse("{}");
    REQUIRE(capped);
    DecodeOptions opts;
    opts.maxErrors = 1;
    auto required = a.decode(*capped, opts);
    REQUIRE_FALSE(required);
    CHECK(required.error().code == "schema.invalid");
    CHECK(required.error().details.back().code == "schema.too_many_errors");
    auto check = number()
                     .check([](double numberValue, CheckCtx& c) {
                         if (numberValue > 0)
                             c.error("schema.check.positive", "positive forbidden", "use zero");
                     })
                     .decode(json::Value(1.0));
    REQUIRE_FALSE(check);
    CHECK(check.error().code == "schema.check.positive");
}
TEST_CASE("JSON Schema golden") {
    struct Config {
        double cutoff = 0;
        std::string mode;
    };
    auto schema = object<Config>("config")
                      .doc("Filter configuration")
                      .field("cutoff", &Config::cutoff, number().range(20, 20000).unit(Unit::Hz))
                      .defaultValue(1000.0)
                      .field("mode", &Config::mode, string())
                      .build();
    std::ifstream in(std::filesystem::path(__FILE__).parent_path() / "config-schema.golden.json");
    std::string expected((std::istreambuf_iterator<char>(in)), {});
    CHECK(json::writeCanonical(schema.jsonSchema()) == expected);
}
TEST_CASE("float field binding") {
    struct Gain {
        float level = 0;
    };
    auto schema = object<Gain>("gain")
                      .field("level", &Gain::level, number().range(-60, 12).unit(Unit::dB))
                      .build();
    auto input = json::parse("{\"level\":\"-3 dB\"}");
    REQUIRE(input);
    auto decoded = schema.decode(*input);
    REQUIRE(decoded);
    CHECK(decoded->level == -3.0f);
    CHECK(schema.encode(*decoded).find("level")->asDouble() == -3.0);
}
TEST_CASE("document IDs and version policy") {
    struct Config {
        double frequency = 0;
    };
    auto body = object<Config>("config")
                    .field("frequency", &Config::frequency, number().unit(Unit::Hz))
                    .build();
    DocumentType<Config> doc("config", 1, 0, body);
    auto parsedId = parseDocumentId("supple.config/1.0");
    REQUIRE(parsedId);
    CHECK(formatDocumentId(*parsedId) == "supple.config/1.0");
    auto invalidId = parseDocumentId("supple.config/0.x");
    REQUIRE_FALSE(invalidId);
    CHECK(invalidId.error().code == "document.bad_id");
    auto parsed = doc.parse("{\"frequency\":\"1 kHz\",\"schema\":\"supple.config/1.0\"}");
    REQUIRE(parsed);
    CHECK(parsed->frequency == 1000);
    auto saved = doc.serialize(*parsed);
    CHECK(saved.find("\"schema\"") < saved.find("\"frequency\""));
    auto roundTrip = doc.parse(saved);
    REQUIRE(roundTrip);
    CHECK(roundTrip->frequency == 1000);
    auto exportSchema = doc.jsonSchema();
    CHECK(exportSchema.find("properties")->asObject().begin()->key == "schema");
    CHECK(exportSchema.find("required")->at(0).asString() == "schema");
    std::vector<Warning> warnings;
    auto future = doc.parse("{\"schema\":\"supple.config/1.1\",\"frequency\":1000,\"newField\":2}",
                            &warnings);
    REQUIRE(future);
    CHECK(warnings.size() == 1);
    CHECK(warnings.front().path == "/newField");
    auto strict = doc.parse("{\"schema\":\"supple.config/1.0\",\"frequency\":1000,\"newField\":2}");
    REQUIRE_FALSE(strict);
    CHECK(strict.error().code == "schema.unknown_member");
}
TEST_CASE("document boundary errors and migration") {
    struct Config {
        double frequency = 0;
    };
    auto body = object<Config>("config").field("frequency", &Config::frequency, number()).build();
    DocumentType<Config> current("config", 2, 0, body, {[](json::Value old) -> Result<json::Value> {
                                     old["schema"] = "supple.config/2.0";
                                     return old;
                                 }});
    auto migrated = current.parse("{\"schema\":\"supple.config/1.0\",\"frequency\":500}");
    REQUIRE(migrated);
    CHECK(migrated->frequency == 500);
    struct Bad {
        const char* text;
        const char* code;
    };
    const Bad bad[] = {{"{}", "document.missing_id"},
                       {"{\"schema\":\"bad\"}", "document.bad_id"},
                       {"{\"schema\":\"supple.other/2.0\"}", "document.wrong_type"},
                       {"{\"schema\":\"supple.config/3.0\"}", "document.too_new"}};
    for (const auto& item : bad) {
        auto result = current.parse(item.text);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == item.code);
        CHECK(!result.error().hint.empty());
    }
    DocumentType<Config> missing("config", 2, 0, body);
    auto failure = missing.parse("{\"schema\":\"supple.config/1.0\",\"frequency\":500}");
    REQUIRE_FALSE(failure);
    CHECK(failure.error().code == "document.migration_failed");
    DocumentType<Config> broken("config", 2, 0, body, {[](json::Value) -> Result<json::Value> {
                                    return Error("test.fail", "failed migration");
                                }});
    auto fail = broken.parse("{\"schema\":\"supple.config/1.0\",\"frequency\":500}");
    REQUIRE_FALSE(fail);
    CHECK(fail.error().code == "document.migration_failed");
    CHECK(fail.error().details.size() == 1);
}
TEST_CASE("type hints and unit schema export") {
    auto numberSchema = number().range(20, 20000).unit(Unit::Hz);
    auto exported = numberSchema.jsonSchema();
    auto* variants = exported.find("anyOf");
    REQUIRE(variants);
    CHECK(variants->size() == 2);
    CHECK(variants->at(0).find("minimum")->asDouble() == 20);
    CHECK(variants->at(1).find("pattern") != nullptr);
    auto typeError = numberSchema.decode(json::Value(true));
    REQUIRE_FALSE(typeError);
    CHECK(typeError.error().hint.find("20") != std::string::npos);
    auto explicitExample = string().example("example").decode(json::Value(1));
    REQUIRE_FALSE(explicitExample);
    CHECK(explicitExample.error().hint.find("example") != std::string::npos);
}
TEST_CASE("fallback hints list names and examples") {
    struct Small {
        int level = 0;
    };
    auto s = object<Small>("small")
                 .field("level", &Small::level, integerOf<int>().doc("Gain level"))
                 .build();
    auto unknown = json::parse("{\"level\":1,\"xyz\":2}");
    REQUIRE(unknown);
    auto e = s.decode(*unknown);
    REQUIRE_FALSE(e);
    CHECK(e.error().hint.find("level") != std::string::npos);
    auto absent = s.decode(json::Value::object());
    REQUIRE_FALSE(absent);
    CHECK(absent.error().code == "schema.required");
    CHECK(absent.error().hint.find("Gain level") != std::string::npos);
    auto choice = enumeration<int>({{0, "first"}, {1, "second"}});
    auto mismatch = choice.decode(json::Value("xyz"));
    REQUIRE_FALSE(mismatch);
    CHECK(mismatch.error().hint.find("first") != std::string::npos);
    CHECK(mismatch.error().hint.find("second") != std::string::npos);
}
TEST_CASE("default type hint and quantity pattern") {
    struct Cutoff {
        double value = 0;
    };
    auto body = object<Cutoff>("cutoff")
                    .field("value", &Cutoff::value, number().range(20, 20000))
                    .defaultValue(1000.0)
                    .build();
    auto bad = json::parse("{\"value\":true}");
    REQUIRE(bad);
    auto decoded = body.decode(*bad);
    REQUIRE_FALSE(decoded);
    CHECK(decoded.error().code == "schema.type");
    CHECK(decoded.error().hint.find("1000") != std::string::npos);
    auto exported = number().unit(Unit::Hz).jsonSchema();
    const auto& expression = exported.find("anyOf")->at(1).find("pattern")->asString();
    std::regex pattern(expression);
    CHECK(std::regex_match(std::string("1.2 kHz"), pattern));
    CHECK_FALSE(std::regex_match(std::string("2 ms"), pattern));
}

TEST_CASE("R3 integer constraints compare exactly with floating bounds") {
    constexpr std::int64_t b = 9007199254740992LL;
    const auto s = integer();
    CHECK_FALSE(s.max(double(b)).decode(json::Value(static_cast<long long>(b + 1))));
    CHECK_FALSE(s.min(double(-b)).decode(json::Value(static_cast<long long>(-b - 1))));
    CHECK(s.exclusiveMin(double(b)).decode(json::Value(static_cast<long long>(b + 1))));
    CHECK(s.exclusiveMax(double(-b)).decode(json::Value(static_cast<long long>(-b - 1))));
    CHECK_FALSE(s.exclusiveMin(double(b)).decode(json::Value(static_cast<long long>(b))));
    CHECK_FALSE(s.exclusiveMax(double(-b)).decode(json::Value(static_cast<long long>(-b))));
    const auto hi = json::Value(static_cast<long long>(std::numeric_limits<std::int64_t>::max()));
    const auto lo = json::Value(static_cast<long long>(std::numeric_limits<std::int64_t>::min()));
    CHECK(s.exclusiveMax(std::ldexp(1.0, 63)).decode(hi));
    CHECK_FALSE(s.min(std::ldexp(1.0, 63)).decode(hi));
    CHECK(s.min(-std::ldexp(1.0, 63)).decode(lo));
    CHECK_FALSE(s.exclusiveMin(-std::ldexp(1.0, 63)).decode(lo));
    for (double bound : {-1.5, -0.5, 0.5, 1.5}) {
        for (int n = -2; n <= 2; ++n) {
            CHECK(bool(s.min(bound).decode(json::Value(n))) == (n >= bound));
            CHECK(bool(s.max(bound).decode(json::Value(n))) == (n <= bound));
            CHECK(bool(s.exclusiveMin(bound).decode(json::Value(n))) == (n > bound));
            CHECK(bool(s.exclusiveMax(bound).decode(json::Value(n))) == (n < bound));
        }
    }
    CHECK(integerOf<unsigned>().max(-0.5).decode(json::Value(0)).ok() == false);
    CHECK(s.range(-1e100, 1e100).decode(hi));
    CHECK(s.range(-1e100, 1e100).decode(lo));
}

TEST_CASE("string lengths count characters, as the exported JSON Schema does") {
    auto one = string().maxLength(1);
    CHECK(one.decode(json::Value("\xc3\xa9")));          // é: two bytes, one character
    CHECK(one.decode(json::Value("\xf0\x9f\x8e\xb5")));  // 🎵: four bytes
    CHECK_FALSE(one.decode(json::Value("ab")));
    auto three = string().minLength(3);
    CHECK_FALSE(three.decode(json::Value("\xc3\xa9\xc3\xa9")));  // four bytes, two characters
    CHECK(three.decode(json::Value("\xc3\xa9\xc3\xa9\xc3\xa9")));
}

TEST_CASE("array decode replaces a nonempty model default") {
    struct Links {
        std::vector<std::string> values{"volume", "mute", "solo"};
    };
    const auto s = object<Links>("links")
                       .field("values", &Links::values, arrayOf(string()))
                       .defaultValue(Links{}.values)
                       .build();
    auto defaults = s.decode(json::Value::object());
    REQUIRE(defaults);
    CHECK(defaults->values == Links{}.values);
    auto explicitValue = s.decode(json::parse(R"({"values":["pan"]})").value());
    REQUIRE(explicitValue);
    CHECK(explicitValue->values == std::vector<std::string>{"pan"});
    auto empty = s.decode(json::parse(R"({"values":[]})").value());
    REQUIRE(empty);
    CHECK(empty->values.empty());
}
