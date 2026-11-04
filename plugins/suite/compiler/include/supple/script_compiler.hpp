// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The DSP script compiler, for C++ callers (the player, Steppe's commands and
// tests).
//
//   auto result = supple::scriptc::compile(source);
//   if (!result.program) report(result.diagnostics);   // each with a hint
//   result.program->params();  result.program->process();
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace supple::scriptc {

class Program;

struct Message {
    bool error = true;  // false: a warning
    std::string code;   // "script.<area>.<what>"
    std::string message;
    std::string hint;
    int line = 0, column = 0;
};

struct Options {
    bool interpret = false;  // MIR's interpreter rather than generated machine code
    int optimize = 2;        // 0..3
    bool listing = false;    // keep the generated MIR (Program::listing)
};

struct Result {
    std::shared_ptr<const Program> program;  // null when there are errors
    std::vector<Message> diagnostics;
};

Result compile(std::string_view source, const Options& options = {});

// Diagnostics as JSON: [{"severity", "code", "message", "hint", "line", "column"}].
std::string toJson(const std::vector<Message>& diagnostics);
// Diagnostics as compiler-style text: "line:col: error: message (hint)".
std::string toText(const std::vector<Message>& diagnostics, std::string_view file = "script.cpp");

}  // namespace supple::scriptc
