// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The compiler's front door: source text in, a Program and diagnostics out.
#include <cstdio>

#include "program.h"
#include "sema.h"
#include "supple/script_compiler.hpp"

namespace supple::scriptc {

Result compile(std::string_view source, const Options& options) {
    Result result;
    Diagnostics diags;
    Unit unit;
    if (source.size() > (4u << 20)) {
        diags.error(Loc{1, 1}, "script.too_large", "the script is larger than 4 MiB",
                    "keep a script at most 4 MiB: compute tables in prepare() rather than writing "
                    "them out");
    } else {
        std::vector<Token> tokens = lex(source, diags);
        parse(tokens, unit, diags);
        if (!diags.hasErrors()) {
            Analysis analysis;
            analyse(unit, analysis, diags);
            if (!diags.hasErrors()) {
                CompileOptions co;
                co.interpret = options.interpret;
                co.optimize = options.optimize;
                co.listing = options.listing;
                result.program = generate(unit, analysis, co, diags);
                if (diags.hasErrors()) result.program.reset();
            }
        }
    }
    for (const Diagnostic& d : diags.list())
        result.diagnostics.push_back(Message{d.severity == Diagnostic::Severity::Error, d.code,
                                             d.message, d.hint, d.loc.line, d.loc.column});
    return result;
}

namespace {
class ModuleProgram final : public script::detail::ModuleCode {
  public:
    explicit ModuleProgram(std::shared_ptr<const Program> p) : p_(std::move(p)) {}
    const ModuleInfoModel& info() const override { return p_->moduleInfo(); }
    const std::vector<ParamModel>& params() const override { return p_->params(); }
    const std::vector<PortModel>& ports() const override { return p_->ports(); }
    const std::vector<SharedModel>& shared() const override { return p_->shared(); }
    const std::vector<FactoryPatchModel>& factoryPatches() const override {
        return p_->factoryPatches();
    }
    void* create() const override { return p_->create(); }
    void destroy(void* self) const override { p_->destroy(self); }
    void copy(void* dst, const void* src) const override { p_->copy(dst, src); }
    Hook prepare() const override { return p_->prepare(); }
    Hook update() const override { return p_->update(); }
    ProcessHook process() const override { return p_->moduleProcess(); }
    UiHook panel() const override { return p_->panel(); }
    UiHook draw() const override { return p_->draw(); }
    UiHook pointer() const override { return p_->pointer(); }

  private:
    std::shared_ptr<const Program> p_;
};
}  // namespace

std::shared_ptr<const script::detail::ModuleCode> moduleCode(std::shared_ptr<const Program> p) {
    if (!p || p->kind() != PluginKind::Module) return nullptr;
    return std::make_shared<ModuleProgram>(std::move(p));
}

namespace {
std::string quoted(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out + "\"";
}
}  // namespace

std::string toJson(const std::vector<Message>& diagnostics) {
    std::string out = "[";
    for (size_t i = 0; i < diagnostics.size(); ++i) {
        const Message& m = diagnostics[i];
        if (i) out += ",";
        out += "{\"severity\":" + quoted(m.error ? "error" : "warning") +
               ",\"code\":" + quoted(m.code) + ",\"message\":" + quoted(m.message) +
               ",\"hint\":" + quoted(m.hint) + ",\"line\":" + std::to_string(m.line) +
               ",\"column\":" + std::to_string(m.column) + "}";
    }
    return out + "]";
}

std::string toText(const std::vector<Message>& diagnostics, std::string_view file) {
    std::string out;
    for (const Message& m : diagnostics) {
        out += std::string(file) + ":" + std::to_string(m.line) + ":" + std::to_string(m.column) +
               ": " + (m.error ? "error: " : "warning: ") + m.message + " [" + m.code + "]\n";
        if (!m.hint.empty()) out += "    hint: " + m.hint + "\n";
    }
    return out;
}

}  // namespace supple::scriptc
