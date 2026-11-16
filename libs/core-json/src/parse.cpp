// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/json/parse.h>
#include <supple/json/utf8.h>
#include <charconv>
#include <cmath>
#include <limits>
#include "supple/base/chars.h"
namespace supple::json {
namespace {
void appendUtf8(std::string& s, unsigned cp) {
    if (cp < 0x80)
        s += static_cast<char>(cp);
    else if (cp < 0x800) {
        s += static_cast<char>(0xc0 | (cp >> 6));
        s += static_cast<char>(0x80 | (cp & 63));
    } else if (cp < 0x10000) {
        s += static_cast<char>(0xe0 | (cp >> 12));
        s += static_cast<char>(0x80 | ((cp >> 6) & 63));
        s += static_cast<char>(0x80 | (cp & 63));
    } else {
        s += static_cast<char>(0xf0 | (cp >> 18));
        s += static_cast<char>(0x80 | ((cp >> 12) & 63));
        s += static_cast<char>(0x80 | ((cp >> 6) & 63));
        s += static_cast<char>(0x80 | (cp & 63));
    }
}
class Parser {
  public:
    Parser(std::string_view t, const ParseOptions& o) : text_(t), options_(o) {}
    Result<Value> run() {
        if (text_.size() > options_.maxBytes)
            return fail("too_large", "JSON input exceeds byte limit", "reduce the document size");
        if (text_.substr(0, 3) == "\xef\xbb\xbf")
            return fail("bom", "UTF-8 BOM is not allowed", "remove the BOM");
        if (auto bad = firstInvalidUtf8(text_); bad != std::string_view::npos) {
            // Report the first invalid byte rather than letting a string consume it.
            pos_ = bad;
            return fail("invalid_utf8", "invalid UTF-8 input",
                        "encode the document as valid UTF-8");
        }
        skip();
        auto v = value(0);
        if (!v) return v.error();
        skip();
        if (pos_ != text_.size()) {
            if (text_.substr(pos_, 2) == "//" || text_.substr(pos_, 2) == "/*")
                return fail("comment", "comments are not JSON", "use a \"notes\" member instead");
            return fail("trailing_content", "content after JSON value",
                        "remove content after the value");
        }
        return v;
    }

  private:
    std::string_view text_;
    const ParseOptions& options_;
    std::size_t pos_ = 0;
    std::size_t values_ = 0;
    char peek() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }
    void skip() {
        while (peek() == ' ' || peek() == '\t' || peek() == '\r' || peek() == '\n') ++pos_;
    }
    Error fail(std::string_view code, std::string message, std::string hint) const {
        int line = 1, col = 1;
        std::size_t start = 0;
        for (std::size_t i = 0; i < pos_ && i < text_.size(); ++i) {
            if (text_[i] == '\n') {
                ++line;
                col = 1;
                start = i + 1;
            } else
                ++col;
        }
        std::size_t end = text_.find('\n', start);
        if (end == std::string_view::npos) end = text_.size();
        std::size_t excerptStart = pos_ > 40 ? std::max(start, pos_ - 40) : start;
        std::size_t excerptEnd = std::min(end, excerptStart + 80);
        message += "\n    " + std::string(text_.substr(excerptStart, excerptEnd - excerptStart)) +
                   "\n    " + std::string(pos_ - excerptStart, ' ') + '^';
        return Error("json.parse." + std::string(code), std::move(message), {}, std::move(hint))
            .withLoc({line, col});
    }
    Result<unsigned> hex4() {
        if (pos_ + 4 > text_.size())
            return fail("invalid_unicode", "incomplete Unicode escape",
                        "use four hexadecimal digits");
        unsigned cp = 0;
        for (int n = 0; n < 4; ++n) {
            char c = text_[pos_++];
            cp <<= 4;
            if (c >= '0' && c <= '9')
                cp += static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f')
                cp += static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                cp += static_cast<unsigned>(c - 'A' + 10);
            else
                return fail("invalid_unicode", "invalid Unicode escape",
                            "use four hexadecimal digits");
        }
        return cp;
    }
    Result<std::string> string() {
        ++pos_;
        std::string out;
        while (pos_ < text_.size()) {
            char c = text_[pos_++];
            if (c == '"') return out;
            if (static_cast<unsigned char>(c) < 0x20) {
                --pos_;
                return fail("control_char_in_string", "unescaped control character in string",
                            "escape the control character");
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ == text_.size())
                return fail("invalid_escape", "incomplete string escape",
                            "use a JSON escape sequence");
            c = text_[pos_++];
            switch (c) {
                case '"':
                    out += '"';
                    break;
                case '\\':
                    out += '\\';
                    break;
                case '/':
                    out += '/';
                    break;
                case 'b':
                    out += '\b';
                    break;
                case 'f':
                    out += '\f';
                    break;
                case 'n':
                    out += '\n';
                    break;
                case 'r':
                    out += '\r';
                    break;
                case 't':
                    out += '\t';
                    break;
                case 'u': {
                    auto first = hex4();
                    if (!first) return first.error();
                    unsigned cp = *first;
                    if (cp >= 0xd800 && cp <= 0xdbff) {
                        if (text_.substr(pos_, 2) != "\\u")
                            return fail("invalid_unicode", "unpaired high surrogate",
                                        "add a low surrogate escape");
                        pos_ += 2;
                        auto second = hex4();
                        if (!second) return second.error();
                        if (*second < 0xdc00 || *second > 0xdfff)
                            return fail("invalid_unicode", "invalid low surrogate",
                                        "use a low surrogate in DC00-DFFF");
                        cp = 0x10000 + ((cp - 0xd800) << 10) + (*second - 0xdc00);
                    } else if (cp >= 0xdc00 && cp <= 0xdfff)
                        return fail("invalid_unicode", "unpaired low surrogate",
                                    "add a high surrogate escape first");
                    appendUtf8(out, cp);
                    break;
                }
                default:
                    --pos_;
                    return fail("invalid_escape", "invalid string escape",
                                "use a JSON escape such as \\n or \\u0041");
            }
        }
        return fail("unexpected_end", "unterminated string", "close the string with a quote");
    }
    Result<Value> number() {
        std::size_t start = pos_;
        if (peek() == '-') ++pos_;
        if (peek() == '0') {
            ++pos_;
            if (peek() >= '0' && peek() <= '9')
                return fail("invalid_number", "leading zero in number", "remove the leading zero");
        } else {
            if (peek() < '1' || peek() > '9')
                return fail("invalid_number", "invalid number", "write a valid JSON number");
            while (peek() >= '0' && peek() <= '9') ++pos_;
        }
        bool floating = false;
        if (peek() == '.') {
            floating = true;
            ++pos_;
            if (peek() < '0' || peek() > '9')
                return fail("invalid_number", "missing fraction digits",
                            "add digits after the decimal point");
            while (peek() >= '0' && peek() <= '9') ++pos_;
        }
        if (peek() == 'e' || peek() == 'E') {
            floating = true;
            ++pos_;
            if (peek() == '+' || peek() == '-') ++pos_;
            if (peek() < '0' || peek() > '9')
                return fail("invalid_number", "missing exponent digits", "add exponent digits");
            while (peek() >= '0' && peek() <= '9') ++pos_;
        }
        auto literal = text_.substr(start, pos_ - start);
        if (!floating) {
            std::int64_t n;
            auto [p, ec] = std::from_chars(literal.data(), literal.data() + literal.size(), n);
            if (ec == std::errc{} && p == literal.data() + literal.size())
                return Value(static_cast<long long>(n));
        }
        double d = 0;
        auto [p, ec] = ::supple::fromChars(literal.data(), literal.data() + literal.size(), d);
        if (ec != std::errc{} || p != literal.data() + literal.size() || !std::isfinite(d))
            return fail("number_out_of_range", "number is out of range",
                        "use a smaller finite number");
        return Value(d);
    }
    Result<Value> value(std::size_t depth) {
        if (depth > options_.maxDepth)
            return fail("too_deep", "JSON nesting is too deep", "reduce nesting depth");
        if (pos_ == text_.size())
            return fail("unexpected_end", "expected a JSON value", "add a value");
        if (options_.maxValues && values_ >= options_.maxValues)
            return fail("too_many_values", "JSON value budget exceeded",
                        "reduce the number of values in the frame");
        ++values_;  // before constructing the next string, container or primitive
        char c = peek();
        if (c == '"') {
            auto s = string();
            if (!s) return s.error();
            return Value(std::move(*s));
        }
        if (c == '[') {
            ++pos_;
            skip();
            Array a;
            if (peek() == ']') {
                ++pos_;
                return Value(std::move(a));
            }
            while (true) {
                auto v = value(depth + 1);
                if (!v) return v.error();
                a.push_back(std::move(*v));
                skip();
                if (peek() == ']') {
                    ++pos_;
                    return Value(std::move(a));
                }
                if (peek() != ',')
                    return fail("expected_comma_or_end", "expected ',' or ']' after array item",
                                "separate array items with commas");
                ++pos_;
                skip();
                if (peek() == ']')
                    return fail("trailing_comma", "trailing comma in array",
                                "remove the comma before ']'");
            }
        }
        if (c == '{') {
            ++pos_;
            skip();
            Object o;
            if (peek() == '}') {
                ++pos_;
                return Value(std::move(o));
            }
            while (true) {
                if (peek() == '\'')
                    return fail("single_quotes", "single-quoted object key is not JSON",
                                "use double quotes");
                if (peek() != '"')
                    return fail("expected_key", "expected a quoted object key",
                                "quote the member name");
                auto k = string();
                if (!k) return k.error();
                skip();
                if (peek() != ':')
                    return fail("expected_colon", "expected ':' after member \"" + *k + "\"",
                                "add a colon after the key");
                ++pos_;
                skip();
                auto v = value(depth + 1);
                if (!v) return v.error();
                if (o.contains(*k))
                    return fail("duplicate_key", "duplicate object key \"" + *k + "\"",
                                "remove or rename the duplicate key");
                o.set(*k, std::move(*v));
                skip();
                if (peek() == '}') {
                    ++pos_;
                    return Value(std::move(o));
                }
                if (peek() != ',')
                    return fail("expected_comma_or_end",
                                "expected ',' or '}' after member \"" + *k + "\"",
                                "separate object members with commas");
                ++pos_;
                skip();
                if (peek() == '}')
                    return fail("trailing_comma", "trailing comma in object",
                                "remove the comma before '}'");
            }
        }
        if (text_.substr(pos_, 9) == "-Infinity")
            return fail("nan_or_infinity", "non-finite number is not JSON", "use a finite number");
        if (c == '-' || (c >= '0' && c <= '9')) return number();
        for (auto [word, v] : {std::pair<std::string_view, Value>{"true", true},
                               {"false", false},
                               {"null", nullptr}})
            if (text_.substr(pos_, word.size()) == word) {
                pos_ += word.size();
                return v;
            }
        if (c == '\'')
            return fail("single_quotes", "single-quoted string is not JSON", "use double quotes");
        if (text_.substr(pos_, 2) == "//" || text_.substr(pos_, 2) == "/*")
            return fail("comment", "comments are not JSON", "use a \"notes\" member instead");
        if (text_.substr(pos_, 3) == "NaN" || text_.substr(pos_, 8) == "Infinity")
            return fail("nan_or_infinity", "non-finite number is not JSON", "use a finite number");
        return fail("unexpected_char", "unexpected character where a JSON value was expected",
                    "write a valid JSON value");
    }
};
}  // namespace
Result<Value> parse(std::string_view text, const ParseOptions& options) {
    return Parser(text, options).run();
}
}  // namespace supple::json
