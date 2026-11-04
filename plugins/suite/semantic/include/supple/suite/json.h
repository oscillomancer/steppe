// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// JSON for the suite layer: values, a strict parser whose errors say how to
// fix the text, canonical writing (identical content, identical bytes), RFC
// 7396 merge patches and JSON Pointer escaping. Self-contained, because the
// suite ships in exported products. Never on the audio thread.
//
// Canonical text follows Steppe's format rules (ADR-0009): two-space indent,
// members one per line in insertion order, arrays of scalars on one line
// when they fit in 100 columns, integers as integers, doubles as the
// shortest text that reads back exactly (always with a '.' or an exponent),
// and a trailing newline at the top level.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace supple::suite::json {

enum class Type : uint8_t { Null, Bool, Int, Double, String, Array, Object };

// An error a caller can act on: a stable code, a message, a JSON Pointer to
// what is wrong, and a hint saying how to fix it. Parse errors also give the
// 1-based line and byte column.
struct Error {
    std::string code;
    std::string message;
    std::string path;
    std::string hint;
    std::vector<Error> details;
    int line = 0, column = 0;
};

class Value;
class Object;
using Array = std::vector<Value>;

class Value {
  public:
    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool b) : type_(Type::Bool), bool_(b) {}
    Value(int i) : type_(Type::Int), int_(i) {}
    Value(long long i) : type_(Type::Int), int_(i) {}
    Value(double d) : type_(Type::Double), double_(d) {}
    Value(const char* s) : type_(Type::String), string_(s) {}
    Value(std::string s) : type_(Type::String), string_(std::move(s)) {}
    Value(std::string_view s) : type_(Type::String), string_(s) {}
    Value(Array a);
    Value(Object o);
    Value(const Value& other);
    Value(Value&& other) noexcept = default;
    Value& operator=(const Value& other);
    Value& operator=(Value&& other) noexcept = default;
    ~Value();

    static Value array();
    static Value object();

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isInt() const { return type_ == Type::Int; }
    bool isDouble() const { return type_ == Type::Double; }
    bool isNumber() const { return isInt() || isDouble(); }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    // Each reads its own type; anything else gives false, 0 or empty.
    bool asBool() const { return isBool() && bool_; }
    long long asInt() const;  // a whole Double converts; others 0
    double asDouble() const;  // an Int converts
    const std::string& asString() const;
    const Array& asArray() const;
    Array& asArray();
    const Object& asObject() const;
    Object& asObject();

    // Conveniences: find() on a non-object is null; operator[] makes a null
    // value an object and inserts a null member when the key is missing.
    const Value* find(std::string_view key) const;
    Value& operator[](std::string_view key);
    void push_back(Value v);  // a null value becomes an array
    size_t size() const;

    // Deep equality: numbers by value across Int and Double, objects
    // regardless of member order.
    friend bool operator==(const Value& a, const Value& b);

  private:
    Type type_ = Type::Null;
    bool bool_ = false;
    long long int_ = 0;
    double double_ = 0.0;
    std::string string_;
    std::unique_ptr<Array> array_;
    std::unique_ptr<Object> object_;
};

using Member = std::pair<std::string, Value>;

// An object keeps its members in insertion order.
class Object {
  public:
    size_t size() const { return members_.size(); }
    bool empty() const { return members_.empty(); }
    const Value* find(std::string_view key) const;
    Value* find(std::string_view key);
    // Replaces the value of an existing key in place, or appends.
    Value& set(std::string key, Value value);
    bool erase(std::string_view key);
    std::vector<Member>::const_iterator begin() const { return members_.begin(); }
    std::vector<Member>::const_iterator end() const { return members_.end(); }
    std::vector<Member>::iterator begin() { return members_.begin(); }
    std::vector<Member>::iterator end() { return members_.end(); }

  private:
    friend struct ParserObjectAccess;
    std::vector<Member> members_;
};

inline const Value* Value::find(std::string_view key) const {
    return isObject() ? object_->find(key) : nullptr;
}
inline Value Value::array() { return Value(Array{}); }
inline Value Value::object() { return Value(Object{}); }

std::string_view typeName(
    Type type);  // "null", "boolean", "integer", "number", "string", "array", "object"

// Strict RFC 8259 over UTF-8. Integers that fit in 64 bits are Int; other
// numbers Double. False with `error` set (codes json.parse.*) otherwise.
bool parse(std::string_view text, Value& out, Error& error, size_t maxDepth = 256);

std::string writeCanonical(const Value& value);  // pretty, ending in a newline
std::string writeCompact(const Value& value);    // one line, no spaces
std::string formatDouble(double value);          // shortest text that reads back exactly

// RFC 7396: `patch` merged into `target`.
Value mergePatch(const Value& target, const Value& patch);

// RFC 6901: '~' as "~0" and '/' as "~1"; joined onto a pointer.
std::string pointerEscape(std::string_view token);
std::string pointerJoin(std::string_view base, std::string_view key);

// [{code, message, path, hint, details}], compact: what set_patch reports.
std::string errorsToJson(const std::vector<Error>& errors);

}  // namespace supple::suite::json
