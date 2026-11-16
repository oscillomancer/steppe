// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include <supple/json/json.h>
#include "native_writer.h"
namespace supple::mcp::detail {
// Compact JSON bytes and the same bytes escaped inside a JSON string. Count
// before allocating text/structured duplication or the final wire packet.
struct WireSize {
    bool boundQuoted = true;
    std::size_t plain = 0, quoted = 0;
    bool add(std::size_t p, std::size_t q) {
        if (p > NativeWriter::kCapacity - plain ||
            (boundQuoted && q > NativeWriter::kCapacity - quoted))
            return false;
        plain += p;
        quoted += q;
        return true;
    }
    bool string(std::string_view value) {
        if (!add(2, 4)) return false;
        for (std::size_t i = 0; i < value.size(); ++i) {
            const auto c = static_cast<unsigned char>(value[i]);
            if (c >= 0x80) {
                const auto length = json::utf8SequenceLength(value, i);
                if (!add(length ? length : 3, length ? length : 3)) return false;
                if (length) i += length - 1;
            } else if (c == '"' || c == '\\') {
                if (!add(2, 4)) return false;
            } else if (c == '\n' || c == '\t' || c == '\r' || c == '\b' || c == '\f') {
                if (!add(2, 3)) return false;
            } else if (c < 0x20) {
                if (!add(6, 7)) return false;
            } else if (!add(1, 1))
                return false;
        }
        return true;
    }
    bool value(const json::Value& v, unsigned depth = 0) {
        if (depth > 256) return false;
        if (v.isString()) return string(v.asString());
        if (v.isArray()) {
            if (!add(2, 2)) return false;
            bool first = true;
            for (const auto& child : v.asArray()) {
                if (!first && !add(1, 1)) return false;
                first = false;
                if (!value(child, depth + 1)) return false;
            }
            return true;
        }
        if (v.isObject()) {
            if (!add(2, 2)) return false;
            bool first = true;
            for (const auto& field : v.asObject()) {
                if (!first && !add(1, 1)) return false;
                first = false;
                if (!string(field.key) || !add(1, 1) || !value(field.value, depth + 1))
                    return false;
            }
            return true;
        }
        const auto size = json::writeCompact(v).size();
        return add(size, size);
    }
};
inline Error responseTooLarge() {
    return Error("mcp.response_too_large", "response packet exceeds the 64 MiB output budget",
                 "/result",
                 "narrow the query; an edit may already have run, so inspect before retrying");
}
inline bool resultFits(const json::Value& value) {
    WireSize size;
    return size.value(value) && size.quoted <= NativeWriter::kCapacity - 4096 &&
           (!value.isObject() || size.plain <= NativeWriter::kCapacity - 4096 - size.quoted);
}
}  // namespace supple::mcp::detail
