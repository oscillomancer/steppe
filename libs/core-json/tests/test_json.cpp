// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <supple/json/json.h>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>
using namespace supple::json;

TEST_CASE("parser value budget counts containers and values before construction") {
    for (const char* text : {"[1,2,3]", R"({"x":[1,2]})"}) {
        REQUIRE(parse(text, {.maxValues = 4}));
        const auto limited = parse(text, {.maxValues = 3});
        REQUIRE_FALSE(limited);
        CHECK(limited.error().code == "json.parse.too_many_values");
        CHECK_FALSE(limited.error().hint.empty());
        REQUIRE(parse(text));  // trusted default is unchanged
    }
    REQUIRE(parse("{}", {.maxValues = 1}));
    REQUIRE(parse("[]", {.maxValues = 1}));
    CHECK_FALSE(parse("[{}]", {.maxValues = 1}));
}
TEST_CASE("parse and canonical output") {
    auto v = parse("{\"schema\":\"supple.test/1.0\",\"values\":[1,2.0,\"é\"]}");
    REQUIRE(v);
    auto out = writeCanonical(*v);
    CHECK(out.find("2.0") != std::string::npos);
    auto again = parse(out);
    REQUIRE(again);
    CHECK(identical(*again, *v));
    CHECK(writeCanonical(*again) == out);
}
TEST_CASE("parser errors") {
    for (auto [input, code] : {std::pair{"[1,]", "trailing_comma"},
                               {"{a:1}", "expected_key"},
                               {"{\"a\":1,\"a\":2}", "duplicate_key"},
                               {"NaN", "nan_or_infinity"},
                               {"//x", "comment"},
                               {"1e999", "number_out_of_range"}}) {
        auto r = parse(input);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == std::string("json.parse.") + code);
        CHECK(r.error().loc.has_value());
        CHECK(r.error().message.find('^') != std::string::npos);
    }
}
TEST_CASE("pointer merge and diff") {
    auto a = parse("{\"a/b\":1,\"x\":2}");
    REQUIRE(a);
    CHECK(resolve(*a, "/a~1b")->asInt() == 1);
    auto patch = parse("{\"x\":null,\"y\":3}");
    REQUIRE(patch);
    auto b = mergePatch(*a, *patch);
    CHECK(b.find("x") == nullptr);
    CHECK(b.find("y")->asInt() == 3);
    auto created = createMergePatch(*a, b);
    REQUIRE(created);
    CHECK(mergePatch(*a, *created) == b);
    CHECK(diff(*a, b).size() == 2);
}
TEST_CASE("all parse diagnostics") {
    struct Case {
        const char* text;
        const char* code;
    };
    const Case cases[] = {{"", "unexpected_end"},
                          {"?", "unexpected_char"},
                          {"{\"a\":1 \"b\":2}", "expected_comma_or_end"},
                          {"{\"a\" 1}", "expected_colon"},
                          {"{a:1}", "expected_key"},
                          {"[1,]", "trailing_comma"},
                          {"01", "invalid_number"},
                          {"1e999", "number_out_of_range"},
                          {"\"\\q\"", "invalid_escape"},
                          {"\"\\uD800\"", "invalid_unicode"},
                          {"\"\xff\"", "invalid_utf8"},
                          {"\"\n\"", "control_char_in_string"},
                          {"{\"a\":1,\"a\":2}", "duplicate_key"},
                          {"[[0]]", "too_deep"},
                          {"true ", "too_large"},
                          {"true false", "trailing_content"},
                          {"\xef\xbb\xbf{}", "bom"},
                          {"'x'", "single_quotes"},
                          {"// x", "comment"},
                          {"NaN", "nan_or_infinity"}};
    for (const auto& c : cases) {
        ParseOptions options;
        if (std::string(c.code) == "too_deep") options.maxDepth = 1;
        if (std::string(c.code) == "too_large") options.maxBytes = 3;
        auto r = parse(c.text, options);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == std::string("json.parse.") + c.code);
        CHECK(r.error().loc.has_value());
        CHECK(!r.error().hint.empty());
    }
}
TEST_CASE("RFC pointer and merge examples") {
    auto doc = parse("{\"foo\":[\"bar\",\"baz\"],\"\":0,\"a/b\":1,\"m~n\":2}");
    REQUIRE(doc);
    CHECK(resolve(*doc, "") == &*doc);
    CHECK(resolve(*doc, "/foo/0")->asString() == "bar");
    CHECK(resolve(*doc, "/")->asInt() == 0);
    CHECK(resolve(*doc, "/a~1b")->asInt() == 1);
    CHECK(resolve(*doc, "/m~0n")->asInt() == 2);
    CHECK(resolve(*doc, "/foo/01") == nullptr);
    CHECK_FALSE(pointerSplit("/bad~3"));
    auto target = parse("{\"a\":\"b\",\"c\":{\"d\":\"e\",\"f\":\"g\"}}");
    auto patch = parse("{\"a\":\"z\",\"c\":{\"f\":null}}");
    REQUIRE(target);
    REQUIRE(patch);
    CHECK(writeCompact(mergePatch(*target, *patch)) == "{\"a\":\"z\",\"c\":{\"d\":\"e\"}}");
}
TEST_CASE("writer errors and scalar array layout") {
    auto w = write(Value(std::numeric_limits<double>::infinity()));
    CHECK_FALSE(w);
    CHECK(w.error().code == "json.write.not_finite");
    auto a = parse("[1,2,3]");
    REQUIRE(a);
    CHECK(writeCanonical(*a) == "[1, 2, 3]\n");
    WriteOptions options;
    options.maxLineWidth = 5;
    auto longLine = write(*a, options);
    REQUIRE(longLine);
    CHECK(longLine->find("\n") != std::string::npos);
}
TEST_CASE("RFC 8259 corpus") {
    namespace fs = std::filesystem;
    auto root = fs::path(__FILE__).parent_path() / "corpus";
    for (auto kind : {"accept", "reject"})
        for (const auto& entry : fs::directory_iterator(root / kind)) {
            std::ifstream in(entry.path(), std::ios::binary);
            std::string text((std::istreambuf_iterator<char>(in)), {});
            auto r = parse(text);
            INFO(entry.path().string());
            if (std::string(kind) == "accept") {
                REQUIRE(r);
                auto encoded = writeCanonical(*r);
                auto again = parse(encoded);
                REQUIRE(again);
                CHECK(writeCanonical(*again) == encoded);
            } else
                CHECK_FALSE(r);
        }
}
TEST_CASE("merge patch property for document values") {
    std::uint32_t state = 0x12345678;
    auto next = [&]() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    };
    for (int i = 0; i < 1000; ++i) {
        Value a = Value::object(), b = Value::object();
        for (int j = 0; j < 5; ++j) {
            auto key = std::to_string(j);
            if (next() % 3) a[key] = static_cast<int>(next() % 100);
            if (next() % 3) b[key] = static_cast<int>(next() % 100);
        }
        auto patch = createMergePatch(a, b);
        REQUIRE(patch);
        CHECK(mergePatch(a, *patch) == b);
    }
}
TEST_CASE("numeric equality preserves large integer precision") {
    CHECK(Value(1) == Value(1.0));
    CHECK_FALSE(Value(9007199254740993LL) == Value(9007199254740992.0));
    CHECK_FALSE(Value(9007199254740992.0) == Value(9007199254740993LL));
    CHECK(Value(std::numeric_limits<std::int64_t>::min()) == Value(-0x1p63));
    CHECK(Value(-0x1p63) == Value(std::numeric_limits<std::int64_t>::min()));
    CHECK_FALSE(Value(std::numeric_limits<std::int64_t>::max()) == Value(0x1p63));
    CHECK_FALSE(Value(0) == Value(0.5));
    CHECK_FALSE(Value(0) == Value(std::numeric_limits<double>::quiet_NaN()));
    CHECK_FALSE(Value(0) == Value(std::numeric_limits<double>::infinity()));
}
TEST_CASE("strict identity and saturated integers") {
    Value ordered = Value::object();
    ordered["a"] = 1;
    ordered["b"] = 1.0;
    Value reversed = Value::object();
    reversed["b"] = 1.0;
    reversed["a"] = 1;
    CHECK(ordered == reversed);
    CHECK_FALSE(identical(ordered, reversed));
    CHECK(Value(1) == Value(1.0));
    CHECK_FALSE(identical(Value(1), Value(1.0)));
    CHECK_FALSE(identical(Value(-0.0), Value(0.0)));
    CHECK(Value(0x1p63).asInt() == std::numeric_limits<std::int64_t>::max());
    CHECK(Value(-0x1p64).asInt() == std::numeric_limits<std::int64_t>::min());
    CHECK(Value(1.5).asInt() == 0);
}
TEST_CASE("merge patch reports unrepresentable null") {
    auto from = parse("{\"a\":1}");
    auto to = parse("{\"a\":null}");
    REQUIRE(from);
    REQUIRE(to);
    auto patch = createMergePatch(*from, *to);
    REQUIRE_FALSE(patch);
    CHECK(patch.error().code == "json.merge.unrepresentable_null");
    CHECK(patch.error().path == "/a");
    auto arrayFrom = parse("{\"a\":[]}");
    auto arrayTo = parse("{\"a\":[null]}");
    REQUIRE(arrayFrom);
    REQUIRE(arrayTo);
    auto valid = createMergePatch(*arrayFrom, *arrayTo);
    REQUIRE(valid);
    CHECK(mergePatch(*arrayFrom, *valid) == *arrayTo);
}
TEST_CASE("release writer replaces non-finite numbers with null") {
    Value nested = Value::object();
    nested["bad"] = std::numeric_limits<double>::infinity();
    auto safe = write(nested);
    REQUIRE_FALSE(safe);
    CHECK(safe.error().code == "json.write.not_finite");
#ifdef NDEBUG
    CHECK(writeCompact(nested) == "{\"bad\":null}");
    CHECK(writeCanonical(nested) == "{\n  \"bad\": null\n}\n");
#endif
}
TEST_CASE("scalar arrays stay on one line up to exactly the width limit") {
    // Key prefix `  "k": ` is 7 columns; the array is 93 columns: 100 in total.
    std::string items;
    for (int i = 0; i < 31; ++i) items += (i ? ", " : "") + std::string("1");
    // 31 items: 31 digits + 30 * 2 separators + 2 brackets = 93
    auto fits = parse("{\"k\":[" + items + "]}");
    REQUIRE(fits);
    CHECK(writeCanonical(*fits) == "{\n  \"k\": [" + items + "]\n}\n");
    auto over = parse("{\"kk\":[" + items + "]}");
    REQUIRE(over);
    CHECK(writeCanonical(*over).find("[\n") != std::string::npos);
}
TEST_CASE("invalid UTF-8 is located at its first byte") {
    std::string text = "{\"a\": \"ok\", \"b\": \"x\xc3\x28\"}";
    auto r = parse(text);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == "json.parse.invalid_utf8");
    REQUIRE(r.error().loc.has_value());
    CHECK(r.error().loc->column == 20);
}
TEST_CASE("object member index stays exact through every mutation") {
    // Keys are read-only through a mutable iterator, so no edit can leave the
    // index pointing at a renamed member.
    Object probe;
    static_assert(!std::is_assignable_v<decltype((probe.begin()->key)), std::string>);
    static_assert(std::is_assignable_v<decltype((probe.begin()->value)), Value>);

    auto key = [](int i) { return "k" + std::to_string(i); };
    Object o;
    for (int i = 0; i < 40; ++i) o.set(key(i), i);
    for (int i = 0; i < 40; ++i) {
        REQUIRE(o.find(key(i)));
        CHECK(o.find(key(i))->asInt() == i);
    }
    // Erasing shifts later members; lookups follow them and drop below the
    // indexed size as well.
    CHECK(o.erase(key(3)));
    CHECK_FALSE(o.erase(key(3)));
    CHECK_FALSE(o.contains(key(3)));
    CHECK(o.find(key(39))->asInt() == 39);
    for (int i = 0; i < 30; ++i) o.erase(key(i));
    CHECK(o.size() == 10);
    for (int i = 30; i < 40; ++i) CHECK(o.find(key(i))->asInt() == i);
    for (int i = 0; i < 20; ++i) o[key(100 + i)] = i;
    CHECK(o.find(key(119))->asInt() == 19);
    CHECK(o.find(key(35))->asInt() == 35);
    // Sorting, copying and moving carry a usable index.
    o.sortKeys();
    CHECK(o.begin()->key == key(100));
    CHECK(o.find(key(30))->asInt() == 30);
    Object copy = o;
    Object assigned;
    assigned = copy;
    Object moved = std::move(copy);
    for (const Object* each : {&o, &assigned, &moved}) {
        CHECK(each->size() == 30);
        CHECK(each->find(key(110))->asInt() == 10);
        CHECK_FALSE(each->contains(key(3)));
    }
    // Values change through iteration without disturbing lookups.
    for (auto& m : moved) m.value = m.key;
    CHECK(moved.find(key(31))->asString() == key(31));
}
TEST_CASE("concurrent const lookups do not write a shared object") {
    // Under the tsan preset this races if a const lookup builds a cache.
    Value shared = Value::object();
    for (int i = 0; i < 64; ++i) shared["member" + std::to_string(i)] = i;
    const Value& view = shared;
    std::vector<std::thread> readers;
    std::vector<int> found(4, 0);
    for (int t = 0; t < 4; ++t)
        readers.emplace_back([&view, &found, t] {
            for (int i = 0; i < 64; ++i)
                if (const auto* v = view.find("member" + std::to_string(i)); v && v->asInt() == i)
                    ++found[static_cast<std::size_t>(t)];
        });
    for (auto& r : readers) r.join();
    for (int n : found) CHECK(n == 64);
}
TEST_CASE("fragment references resolve JSON pointers against the root") {
    auto doc = parse(R"({"$defs":{"a b":{"x":1},"c/d":2},"list":[5]})");
    REQUIRE(doc);
    CHECK(resolveFragment(*doc, "#") == &*doc);
    REQUIRE(resolveFragment(*doc, "#/$defs/a%20b/x"));
    CHECK(resolveFragment(*doc, "#/$defs/a%20b/x")->asInt() == 1);
    REQUIRE(resolveFragment(*doc, "#/$defs/c~1d"));
    CHECK(resolveFragment(*doc, "#/$defs/c~1d")->asInt() == 2);
    REQUIRE(resolveFragment(*doc, "#/list/0"));
    CHECK(resolveFragment(*doc, "#/list/0")->asInt() == 5);
    CHECK(resolveFragment(*doc, "/$defs") == nullptr);
    CHECK(resolveFragment(*doc, "other.json#/$defs") == nullptr);
    CHECK(resolveFragment(*doc, "#/$defs/a%2") == nullptr);
    CHECK(resolveFragment(*doc, "#/$defs/a%zz") == nullptr);
    CHECK(resolveFragment(*doc, "#/missing") == nullptr);
}

TEST_CASE("the writer replaces bytes that are not UTF-8, so its output parses back") {
    using namespace supple::json;
    const std::string valid = "caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x8e\xb5";  // é € 🎵
    auto kept = parse(writeCompact(Value(valid)));
    REQUIRE(kept);
    CHECK(kept->asString() == valid);
    for (const std::string& bad : {std::string("\xff"), std::string("a\xc3"),
                                   std::string("\xed\xa0\x80"), std::string("\xc0\xaf")}) {
        Value object = Value::object();
        object[bad] = bad;
        auto written = write(object);
        REQUIRE(written);
        auto back = parse(*written);
        REQUIRE_MESSAGE(back, *written);
        CHECK(firstInvalidUtf8(back->asObject().begin()->value.asString()) ==
              std::string_view::npos);
    }
    CHECK(*parse(writeCompact(Value(std::string("a\xff"
                                                "b")))) == Value("a\xef\xbf\xbd"
                                                                 "b"));
}

TEST_CASE("keys that write the same once repaired are refused, or the first kept") {
    using namespace supple::json;
    Value object = Value::object();
    object["\x80"] = 1;
    object["\x81"] = 2;
    object["\xef\xbf\xbd"] = 3;  // U+FFFD itself
    Value nested = Value::array();
    nested.push_back(object);
    for (const Value& v : {object, nested}) {
        auto refused = write(v);
        REQUIRE_FALSE(refused);
        CHECK(refused.error().code == "json.write.duplicate_key");
        CHECK(!refused.error().hint.empty());
    }
    auto compact = parse(writeCompact(object));
    REQUIRE(compact);
    CHECK(compact->asObject().size() == 1);
    CHECK(compact->find("\xef\xbf\xbd")->asInt() == 1);
    CHECK(parse(writeCanonical(nested)));
    Value distinct = Value::object();  // repaired, these stay apart
    distinct["a\x80"] = 1;
    distinct["b\x80"] = 2;
    REQUIRE(write(distinct));
    CHECK(parse(*write(distinct))->asObject().size() == 2);
}
