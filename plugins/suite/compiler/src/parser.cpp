// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Recursive descent over C++'s grammar, for the subset. C++ needs to know
// which names are types to parse a statement; a first pass collects the
// script's own struct and enum names, and the runtime's are fixed. What the
// subset leaves out (pointers, C arrays, constructors, templates, lambdas,
// new, exceptions...) is parsed far enough to say what to write instead.
#include <set>
#include <string>

#include "ast.h"
#include "builtins.h"

namespace supple::scriptc {

namespace {

// The runtime's types, as a script names them: its classes and top-level
// enums (from the registry), and the names it gives no class of its own.
bool isRuntimeType(const std::string& name) {
    static const std::set<std::string> others = {"Array",       "Buffer", "Shared",
                                                 "SharedArray", "Effect", "Instrument",
                                                 "NoteEffect",  "Unit",   "Curve"};
    const Builtins& b = Builtins::get();
    return others.count(name) || name == "Params" || name == "Inputs" || name == "Outputs" ||
           b.cls(name) || b.enumNamed(name);
}
const std::set<std::string>& runtimeTemplates() {
    static const std::set<std::string> names = {"Array",  "Buffer", "Shared", "SharedArray",
                                                "Params", "Inputs", "Outputs"};
    return names;
}
// Types the runtime declares inside its classes: Oscillator::Shape.
const std::set<std::string>& runtimeNestedTypes() {
    static const std::set<std::string> names = {"Shape", "Kind"};
    return names;
}
bool isTypeKeyword(const Token& t) {
    return t.kind == Tok::Keyword &&
           (t.text == "double" || t.text == "float" || t.text == "int" || t.text == "bool" ||
            t.text == "void" || t.text == "auto" || t.text == "char" || t.text == "unsigned" ||
            t.text == "signed" || t.text == "long" || t.text == "short" || t.text == "wchar_t" ||
            t.text == "char8_t" || t.text == "char16_t" || t.text == "char32_t");
}

class Parser {
  public:
    Parser(std::vector<Token>& toks, Unit& unit, Diagnostics& diags)
        : toks_(toks), unit_(unit), diags_(diags) {
        // The script's own type names, wherever they are declared.
        for (size_t i = 0; i + 1 < toks_.size(); ++i) {
            const Token& t = toks_[i];
            if (t.keyword("struct") || t.keyword("class") || t.keyword("union")) {
                if (toks_[i + 1].kind == Tok::Ident) types_.insert(toks_[i + 1].text);
            } else if (t.keyword("enum")) {
                size_t j = i + 1;
                if (toks_[j].keyword("class") || toks_[j].keyword("struct")) ++j;
                if (j < toks_.size() && toks_[j].kind == Tok::Ident) types_.insert(toks_[j].text);
            }
        }
    }

    void run() {
        while (!at(Tok::End)) {
            const size_t before = pos_;
            if (Decl* d = topLevel()) unit_.decls.push_back(d);
            if (pos_ == before) advance();  // never loop on an error
        }
    }

  private:
    // --- tokens ----------------------------------------------------------------

    const Token& cur() const { return toks_[pos_]; }
    const Token& peek(size_t k = 1) const {
        return toks_[pos_ + k < toks_.size() ? pos_ + k : toks_.size() - 1];
    }
    bool at(Tok k) const { return cur().kind == k; }
    bool atPunct(std::string_view p) const { return cur().punct(p); }
    bool atKeyword(std::string_view k) const { return cur().keyword(k); }
    const Token& advance() {
        const Token& t = toks_[pos_];
        if (pos_ + 1 < toks_.size()) ++pos_;
        return t;
    }
    bool accept(std::string_view p) {
        if (atPunct(p)) {
            advance();
            return true;
        }
        return false;
    }
    bool acceptKeyword(std::string_view k) {
        if (atKeyword(k)) {
            advance();
            return true;
        }
        return false;
    }
    void expect(std::string_view p, std::string_view what = {}) {
        if (accept(p)) return;
        if (p == ">" && acceptCloseAngle()) return;
        error(cur().loc, "script.parse.expected",
              "expected '" + std::string(p) + "'" + (what.empty() ? "" : " " + std::string(what)) +
                  " before " + describe(cur()),
              p == ";" ? std::string("add the ';' that ends the statement or declaration")
                       : "add the '" + std::string(p) +
                             "'; a missing ';' or bracket just before is the usual cause");
    }
    // Closes a template argument list. A '>>' closes two: the first is
    // taken and the token becomes the second '>' (C++11's rule).
    bool acceptCloseAngle() {
        if (accept(">")) return true;
        if (atPunct(">>")) {
            toks_[pos_].text = ">";
            toks_[pos_].loc.column += 1;
            return true;
        }
        return false;
    }
    static std::string describe(const Token& t) {
        switch (t.kind) {
            case Tok::End:
                return "the end of the file";
            case Tok::StringLit:
                return "a string";
            case Tok::IntLit:
            case Tok::FloatLit:
                return "'" + t.text + "'";
            default:
                return "'" + t.text + "'";
        }
    }
    void error(Loc loc, std::string code, std::string message, std::string hint) {
        if (abandoned_) return;
        diags_.error(loc, std::move(code), std::move(message), std::move(hint));
    }

    // How deep expressions, statements, initializers and types may nest:
    // the checker and code generator walk them recursively, so source within
    // the size limit could otherwise exhaust the stack. A chain (a + b + c,
    // a.f().g(), a, b) nests one level per link. Past the limit the parse
    // stops with one error.
    static constexpr int kMaxNesting = 256;
    void deeper(int levels = 1) {
        depth_ += levels;
        if (depth_ <= kMaxNesting || abandoned_) return;
        error(cur().loc, "script.nesting", "expressions, blocks or types nest too deeply here",
              "split the expression into named locals, or move inner blocks into functions");
        abandoned_ = true;
        pos_ = toks_.size() - 1;  // the end: every loop stops there
    }
    struct Nest {
        explicit Nest(Parser& parser) : p(parser) { p.deeper(); }
        ~Nest() { --p.depth_; }
        Nest(const Nest&) = delete;
        Nest& operator=(const Nest&) = delete;
        Parser& p;
    };
    // Skips to the end of the current statement or declaration after an error.
    void recover() {
        int depth = 0;
        while (!at(Tok::End)) {
            if (atPunct("{")) ++depth;
            if (atPunct("}")) {
                if (depth == 0) return;
                --depth;
                if (depth == 0) {
                    advance();
                    accept(";");
                    return;
                }
            }
            if (atPunct(";") && depth == 0) {
                advance();
                return;
            }
            advance();
        }
    }

    // --- top level ---------------------------------------------------------------

    Decl* topLevel() {
        const Token& t = cur();
        if (t.kind == Tok::Hash) return preprocessor();
        if (accept(";")) return nullptr;
        if (t.keyword("using")) return usingDecl();
        if (t.keyword("namespace")) {
            error(t.loc, "script.unsupported.namespace", "namespaces are not in the subset",
                  "declare everything at the top level of the script");
            recover();
            return nullptr;
        }
        if (t.keyword("template")) {
            error(t.loc, "script.unsupported.template", "templates are not in the subset",
                  "write the function or struct for the types you need (double, usually)");
            recover();
            return nullptr;
        }
        if (t.keyword("struct") || t.keyword("class") || t.keyword("union")) {
            if (peek().kind == Tok::Ident && (peek(2).punct("{") || peek(2).punct(":")))
                return structDecl(nullptr);
        }
        if (t.keyword("enum")) return enumDecl(nullptr);
        if (t.keyword("typedef")) {
            error(t.loc, "script.unsupported.typedef", "typedef is not in the subset",
                  "write the type itself");
            recover();
            return nullptr;
        }
        if (t.keyword("static_assert")) {
            error(t.loc, "script.unsupported.static_assert", "static_assert is not in the subset",
                  "remove it");
            recover();
            return nullptr;
        }
        return declarationOrFunction(nullptr);
    }

    Decl* preprocessor() {
        const Token& t = advance();
        std::string text = t.text.substr(1);
        size_t a = text.find_first_not_of(" \t");
        text = a == std::string::npos ? "" : text.substr(a);
        if (text.rfind("include", 0) == 0) {
            auto* d = unit_.decl<IncludeDecl>(t.loc);
            std::string rest = text.substr(7);
            size_t s = rest.find_first_not_of(" \t");
            rest = s == std::string::npos ? "" : rest.substr(s);
            size_t comment = rest.find("//");
            if (comment != std::string::npos) rest = rest.substr(0, comment);
            while (!rest.empty() &&
                   (rest.back() == ' ' || rest.back() == '\t' || rest.back() == '\r'))
                rest.pop_back();
            if (rest.size() >= 2 && ((rest.front() == '<' && rest.back() == '>') ||
                                     (rest.front() == '"' && rest.back() == '"'))) {
                d->angled = rest.front() == '<';
                d->header = rest.substr(1, rest.size() - 2);
            } else {
                d->header = rest;
            }
            return d;
        }
        if (text.rfind("pragma once", 0) == 0) return nullptr;
        if (text.rfind("define", 0) == 0) {
            error(t.loc, "script.unsupported.macro", "macros are not in the subset",
                  "use a constant: static constexpr double kName = 1.0;  (or a function)");
            return nullptr;
        }
        error(t.loc, "script.unsupported.preprocessor", "the preprocessor is not in the subset",
              "the only preprocessor line a script has is #include <supple/script.h>");
        return nullptr;
    }

    Decl* usingDecl() {
        const Loc loc = advance().loc;
        auto* d = unit_.decl<UsingDecl>(loc);
        if (acceptKeyword("namespace")) {
            while (at(Tok::Ident)) {
                d->path.push_back(advance().text);
                if (!accept("::")) break;
            }
            expect(";");
            return d;
        }
        error(loc, "script.unsupported.using",
              "type aliases and using-declarations are not in the subset",
              "write the type itself; `using namespace supple::script;` is the one using a script "
              "may have");
        recover();
        return nullptr;
    }

    // --- types -------------------------------------------------------------------

    // True when a type starts here (for telling declarations from expressions).
    bool startsType(size_t at = 0) const {
        const Token& t = peek(at);
        if (isTypeKeyword(t)) return true;
        if (t.keyword("const") || t.keyword("constexpr") || t.keyword("static") ||
            t.keyword("inline") || t.keyword("volatile") || t.keyword("mutable") ||
            t.keyword("thread_local") || t.keyword("extern") || t.keyword("register") ||
            t.keyword("typename"))
            return true;
        if (t.kind == Tok::Ident) {
            // std::array, std::vector and friends: a type, refused later.
            if (t.text == "std" && peek(at + 1).punct("::")) return isStdType(peek(at + 2).text);
            if (types_.count(t.text) || isRuntimeType(t.text)) {
                // A qualified name that ends in a value is an expression:
                // Oscillator::Shape::Saw.
                size_t k = at;
                while (peek(k + 1).punct("::") && peek(k + 2).kind == Tok::Ident) k += 2;
                const Token& after = peek(k + 1);
                if (!isTypeLike(peek(k).text)) return false;
                return after.kind == Tok::Ident || after.punct("&") || after.punct("<") ||
                       after.punct("*") || after.punct("&&") || after.keyword("const") ||
                       after.keyword("operator");
            }
        }
        return false;
    }
    bool isTypeLike(const std::string& name) const {
        return types_.count(name) || isRuntimeType(name) || runtimeNestedTypes().count(name);
    }
    // A template argument that is a type: a type keyword or a type's name.
    bool startsTypeArgument() const {
        const Token& t = cur();
        if (isTypeKeyword(t) || t.keyword("const")) return true;
        if (t.kind != Tok::Ident) return false;
        if (t.text == "std" && peek().punct("::")) return true;
        size_t k = 0;
        while (peek(k + 1).punct("::") && peek(k + 2).kind == Tok::Ident) k += 2;
        return isTypeLike(peek(k).text);
    }
    static bool isStdType(const std::string& n) {
        return n == "array" || n == "vector" || n == "string" || n == "map" ||
               n == "unordered_map" || n == "complex" || n == "atomic" || n == "unique_ptr" ||
               n == "shared_ptr" || n == "function" || n == "size_t" || n == "int32_t" ||
               n == "uint32_t" || n == "int64_t" || n == "uint64_t" || n == "deque" ||
               n == "list" || n == "span" || n == "pair" || n == "tuple" || n == "optional";
    }

    TypeSpec* typeSpec(bool allowStorage = true) {
        const Nest nest(*this);
        TypeSpec* s = unit_.spec();
        s->loc = cur().loc;
        // Leading specifiers.
        while (true) {
            if (acceptKeyword("const"))
                s->isConst = true;
            else if (acceptKeyword("constexpr"))
                s->isConstexpr = true;
            else if (atKeyword("static")) {
                if (!allowStorage)
                    error(cur().loc, "script.parse.static", "static is not allowed here",
                          "remove it");
                advance();
                s->isStatic = true;
            } else if (atKeyword("inline"))
                advance();
            else if (atKeyword("volatile") || atKeyword("mutable") || atKeyword("thread_local") ||
                     atKeyword("extern") || atKeyword("register")) {
                error(cur().loc, "script.unsupported.specifier",
                      "'" + cur().text + "' is not in the subset", "remove it");
                advance();
            } else if (atKeyword("typename"))
                advance();
            else
                break;
        }
        const Token& t = cur();
        if (isTypeKeyword(t)) {
            if (t.text == "auto") {
                s->isAuto = true;
                s->path = {"auto"};
                advance();
            } else if (t.text == "char" && s->isConst && peek().punct("*")) {
                // const char*: text, for the editor.
                s->path = {"const char*"};
                s->isConst = false;
                advance();
                advance();
            } else if (t.text == "unsigned" || t.text == "signed" || t.text == "long" ||
                       t.text == "short" || t.text == "char" || t.text == "wchar_t" ||
                       t.text == "char8_t" || t.text == "char16_t" || t.text == "char32_t") {
                error(t.loc, "script.unsupported.type",
                      "'" + t.text + "' is not a type the subset has",
                      "the numbers are double, float, int and bool: use int for whole numbers, "
                      "double for the rest");
                s->unsupported = true;
                s->path = {"int"};
                while (isTypeKeyword(cur()) && cur().text != "auto") advance();
            } else {
                s->path = {t.text};
                advance();
            }
        } else if (t.kind == Tok::Ident) {
            if (t.text == "std" && peek().punct("::")) {
                advance();
                advance();
                const std::string name = cur().text;
                error(
                    t.loc, "script.unsupported.std",
                    "std::" + name + " is not available in a script",
                    name == "array"    ? "write Array<float, 16> (indexing is checked)"
                    : name == "vector" ? "declare Buffer<float> buffer; and resize it in prepare()"
                    : name == "string"
                        ? "text is only for the editor: a string literal, or format(\"%.1f dB\", x)"
                    : name == "complex" ? "keep the real and imaginary parts in two doubles"
                    : name == "atomic"
                        ? "share values with the editor through a Shared<double> member"
                        : "use int for whole numbers and double for the rest");
                s->unsupported = true;
                s->path = {"int"};
                if (at(Tok::Ident)) advance();
                if (atPunct("<")) skipTemplateArgs();
            } else {
                s->path.push_back(advance().text);
                if (atPunct("<") && runtimeTemplates().count(s->path.back()))
                    templateArgs(s);
                else if (atPunct("<")) {
                    error(cur().loc, "script.unsupported.template",
                          "'" + s->path.back() + "' is not a template",
                          "only Array<T, N>, Buffer<T>, Shared<T> and SharedArray<T, N> take "
                          "template "
                          "arguments");
                    skipTemplateArgs();
                }
                while (atPunct("::") && peek().kind == Tok::Ident) {
                    advance();
                    s->path.push_back(advance().text);
                }
            }
        } else {
            error(t.loc, "script.parse.type", "expected a type before " + describe(t),
                  "a declaration starts with its type: double, float, int, bool, auto, or a "
                  "struct's name");
            s->path = {"int"};
            s->unsupported = true;
            return s;
        }
        // Trailing const (east const), pointers and references.
        while (true) {
            if (acceptKeyword("const"))
                s->isConst = true;
            else if (atPunct("*")) {
                error(cur().loc, "script.unsupported.pointer", "pointers are not in the subset",
                      "use a reference (T&) to refer to an object, or an index into an Array");
                advance();
                s->pointers++;
            } else if (atPunct("&&")) {
                error(cur().loc, "script.unsupported.rvalue_ref",
                      "rvalue references are not in the subset", "use a reference (T&) or a value");
                advance();
                s->rvalueRef = true;
            } else if (atPunct("&")) {
                advance();
                s->isRef = true;
            } else
                break;
        }
        return s;
    }

    void templateArgs(TypeSpec* s) {
        expect("<");
        if (atPunct(">")) {
            advance();
            return;
        }
        while (true) {
            if (startsTypeArgument()) {
                s->typeArgs.push_back(typeSpec(false));
                s->argIsType.push_back(true);
            } else {
                inTemplate_++;
                s->valueArgs.push_back(conditional());
                inTemplate_--;
                s->argIsType.push_back(false);
            }
            if (accept(",")) continue;
            break;
        }
        if (!acceptCloseAngle())
            error(cur().loc, "script.parse.expected",
                  "expected '>' to close the template arguments before " + describe(cur()),
                  "close them with '>': Array<float, 512>");
    }
    void skipTemplateArgs() {
        int depth = 0;
        while (!at(Tok::End)) {
            if (atPunct("<"))
                ++depth;
            else if (atPunct(">")) {
                if (--depth <= 0) {
                    advance();
                    return;
                }
            } else if (atPunct(">>")) {
                depth -= 2;
                if (depth <= 0) {
                    advance();
                    return;
                }
            } else if (atPunct(";") || atPunct("{"))
                return;
            advance();
        }
    }

    // --- declarations -------------------------------------------------------------

    StructDecl* structDecl(StructDecl* outer) {
        const Token& kw = advance();
        auto* s = unit_.decl<StructDecl>(kw.loc);
        s->isClass = kw.text == "class";
        s->outer = outer;
        if (kw.text == "union")
            error(kw.loc, "script.unsupported.union", "unions are not in the subset",
                  "use a struct");
        if (s->isClass)
            error(kw.loc, "script.unsupported.class",
                  "a class's members are private, so code outside it could not use them",
                  "write struct instead of class");
        s->name = advance().text;
        if (accept(":")) {
            acceptKeyword("public");
            if (atKeyword("private") || atKeyword("protected") || atKeyword("virtual")) {
                error(cur().loc, "script.unsupported.inheritance",
                      "only public inheritance from the runtime's bases",
                      "write struct Name : Effect { ... }");
                advance();
            }
            s->baseLoc = cur().loc;
            if (at(Tok::Ident)) {
                s->baseName = advance().text;
                while (accept("::") && at(Tok::Ident)) s->baseName = advance().text;
            }
            if (s->baseName == "Effect")
                s->base = PluginKind::Effect;
            else if (s->baseName == "Instrument")
                s->base = PluginKind::Instrument;
            else if (s->baseName == "Module")
                s->base = PluginKind::Module;
            else if (s->baseName == "NoteEffect")
                s->base = PluginKind::NoteEffect;
            else
                error(s->baseLoc, "script.unsupported.inheritance",
                      "a struct can only derive from Effect, Instrument, NoteEffect or Module",
                      "keep shared parts as a member instead: struct Voice { ... }; Array<Voice, "
                      "8> voices;");
            if (accept(",")) {
                error(cur().loc, "script.unsupported.inheritance", "a struct has at most one base",
                      "derive the plugin from one: struct Tilt : Effect { ... }");
                while (!atPunct("{") && !at(Tok::End)) advance();
            }
        }
        expect("{", "to open the struct");
        while (!atPunct("}") && !at(Tok::End)) {
            const size_t before = pos_;
            member(s);
            if (pos_ == before) advance();
        }
        expect("}", "to close the struct");
        if (at(Tok::Ident)) {
            error(cur().loc, "script.parse.struct_variable",
                  "declare the variable separately from the struct",
                  "struct Voice { ... };  then  Voice voice;");
            while (!atPunct(";") && !at(Tok::End)) advance();
        }
        expect(";", "after the struct");
        return s;
    }

    void member(StructDecl* s) {
        const Token& t = cur();
        if (accept(";")) return;
        if (t.keyword("public") && peek().punct(":")) {
            advance();
            advance();
            return;
        }
        if ((t.keyword("private") || t.keyword("protected")) && peek().punct(":")) {
            error(t.loc, "script.unsupported.access", "every member of a script's struct is public",
                  "remove '" + t.text + ":'");
            advance();
            advance();
            return;
        }
        if (t.keyword("struct") || t.keyword("class") || t.keyword("union")) {
            if (peek().kind == Tok::Ident && (peek(2).punct("{") || peek(2).punct(":"))) {
                s->nested.push_back(structDecl(s));
                return;
            }
        }
        if (t.keyword("enum")) {
            if (auto* e = enumDecl(s)) s->enums.push_back(e);
            return;
        }
        if (t.keyword("virtual") || t.keyword("friend") || t.keyword("using") ||
            t.keyword("template") || t.keyword("typedef") || t.keyword("explicit") ||
            t.keyword("static_assert") || t.keyword("operator")) {
            error(t.loc, "script.unsupported.member", "'" + t.text + "' is not in the subset",
                  "remove it");
            recover();
            return;
        }
        // A constructor or destructor.
        if ((t.kind == Tok::Ident && t.text == s->name && peek().punct("(")) ||
            (t.punct("~") && peek().text == s->name)) {
            error(t.loc, "script.unsupported.constructor",
                  t.punct("~") ? "destructors are not in the subset"
                               : "constructors are not in the subset",
                  t.punct("~") ? "built-in objects release what they hold by themselves"
                               : "give members default values (double gain = 1.0;) and set the "
                                 "rest up in prepare()");
            recover();
            return;
        }
        Decl* d = declarationOrFunction(s);
        (void)d;
    }

    EnumDecl* enumDecl(StructDecl* outer) {
        const Loc loc = advance().loc;
        auto* e = unit_.decl<EnumDecl>(loc);
        e->outer = outer;
        if (acceptKeyword("class") || acceptKeyword("struct")) {
            e->scoped = true;
        } else {
            e->scoped = false;
            error(loc, "script.unsupported.enum", "plain enums are not in the subset",
                  "write enum class, and name its values as Name::Value");
        }
        if (at(Tok::Ident)) e->name = advance().text;
        if (accept(":")) {
            TypeSpec* under = typeSpec(false);
            if (under->path.empty() || under->path[0] != "int")
                error(under->loc, "script.unsupported.enum_base",
                      "an enum's underlying type is int",
                      "remove ': " + (under->path.empty() ? std::string() : under->path[0]) + "'");
        }
        expect("{", "to open the enum");
        while (at(Tok::Ident)) {
            std::string name = advance().text;
            Expr* value = nullptr;
            if (accept("=")) value = conditional();
            e->enumerators.emplace_back(std::move(name), value);
            if (!accept(",")) break;
        }
        expect("}", "to close the enum");
        expect(";", "after the enum");
        if (outer == nullptr) unit_.decls.push_back(e);
        return outer ? e : nullptr;
    }

    // A variable, constant or function at the top level or in a struct.
    Decl* declarationOrFunction(StructDecl* owner) {
        const Loc start = cur().loc;
        if (!startsType() && !(cur().kind == Tok::Ident && peek().kind == Tok::Ident)) {
            if (cur().kind == Tok::Ident && peek().punct("("))
                error(cur().loc, "script.parse.declaration",
                      "'" + cur().text +
                          "(' at the top level: a function needs its result type first",
                      "write void " + cur().text + "(...) { ... } or double " + cur().text +
                          "(...) { ... }");
            else
                error(cur().loc, "script.parse.declaration",
                      "expected a declaration before " + describe(cur()),
                      owner ? "a struct holds members (double gain = 1.0;) and member functions"
                            : "the top level holds structs, functions, enums and constants");
            recover();
            return nullptr;
        }
        TypeSpec* type = typeSpec(true);
        if (at(Tok::Keyword) && cur().text == "operator") {
            error(cur().loc, "script.unsupported.operator",
                  "operator overloading is not in the subset", "write a named function instead");
            recover();
            return nullptr;
        }
        if (!at(Tok::Ident)) {
            error(cur().loc, "script.parse.name", "expected a name before " + describe(cur()),
                  "write the type, then the name: double level = 0.0;");
            recover();
            return nullptr;
        }
        const Token& nameTok = advance();
        if (atPunct("::")) {
            error(nameTok.loc, "script.unsupported.out_of_line",
                  "member functions are defined inside their struct",
                  "move the body into struct " + nameTok.text + " { ... }");
            recover();
            return nullptr;
        }
        if (atPunct("(")) return functionDecl(type, nameTok, owner);
        // Data: one or more declarators.
        std::vector<Decl*> made;
        std::string name = nameTok.text;
        Loc nameLoc = nameTok.loc;
        while (true) {
            Expr* init = declaratorInit(type, name);
            if (owner) {
                auto* f = unit_.decl<FieldDecl>(nameLoc);
                f->name = name;
                f->spec = type;
                f->init = init;
                f->owner = owner;
                f->isStatic = type->isStatic;
                owner->fields.push_back(f);
                f->oneDeclarator = made.empty();
                made.push_back(f);
            } else {
                auto* v = unit_.decl<VarDecl>(nameLoc);
                v->name = name;
                v->spec = type;
                v->init = init;
                v->isGlobal = true;
                made.push_back(v);
                unit_.decls.push_back(v);
            }
            if (!accept(",")) break;
            if (owner)
                for (Decl* d : made) static_cast<FieldDecl*>(d)->oneDeclarator = false;
            if (!at(Tok::Ident)) {
                error(cur().loc, "script.parse.name", "expected a name after ','",
                      "name each one: double a = 0.0, b = 0.0;");
                break;
            }
            const Token& next = advance();
            name = next.text;
            nameLoc = next.loc;
        }
        expect(";", "after the declaration");
        (void)start;
        return nullptr;
    }

    // What follows a declarator's name: = value, { value }, nothing; and
    // what the subset refuses there (C arrays, bit-fields, T x(...)).
    Expr* declaratorInit(TypeSpec* type, const std::string& name) {
        if (atPunct("[")) {
            const Loc loc = cur().loc;
            std::string n;
            advance();
            if (at(Tok::IntLit)) n = cur().text;
            while (!atPunct("]") && !at(Tok::End)) advance();
            accept("]");
            const std::string elem = type->path.empty() ? "double" : type->path.back();
            error(loc, "script.unsupported.c_array",
                  "C arrays are not checked, so the subset leaves them out",
                  "write Array<" + elem + ", " + (n.empty() ? "N" : n) + "> " + name +
                      ";  (an out-of-range index uses a spare element and is reported)");
            while (atPunct("[")) {
                while (!atPunct("]") && !at(Tok::End)) advance();
                accept("]");
            }
        }
        if (atPunct(":")) {
            error(cur().loc, "script.unsupported.bitfield", "bit-fields are not in the subset",
                  "use an int or a bool");
            advance();
            conditional();
        }
        if (accept("=")) {
            if (atPunct("{")) return initList();
            return assignment();
        }
        if (atPunct("{")) return initList();
        if (atPunct("(")) {
            const Loc loc = cur().loc;
            advance();
            if (accept(")")) {
                error(loc, "script.parse.vexing",
                      "'" + name + "()' declares a function, not a variable, in C++",
                      "write " + name + "; or " + name + "{};");
                return nullptr;
            }
            Expr* e = assignment();
            if (accept(",")) {
                error(loc, "script.unsupported.constructor_call",
                      "objects are not constructed with arguments",
                      "declare it and set it up in prepare()");
                while (!atPunct(")") && !at(Tok::End)) advance();
            }
            expect(")");
            return e;
        }
        return nullptr;
    }

    FunctionDecl* functionDecl(TypeSpec* result, const Token& name, StructDecl* owner) {
        auto* f = unit_.decl<FunctionDecl>(name.loc);
        f->name = name.text;
        f->result = result;
        f->owner = owner;
        f->isStatic = result->isStatic;
        f->isConstexpr = result->isConstexpr;
        if (result->isStatic && owner)
            error(result->loc, "script.unsupported.static_method",
                  "static member functions are not in the subset",
                  "make it a member function, or a function outside the struct");
        expect("(");
        if (atKeyword("void") && peek().punct(")")) advance();
        while (!atPunct(")") && !at(Tok::End)) {
            auto* p = unit_.decl<VarDecl>(cur().loc);
            p->isParam = true;
            p->spec = typeSpec(false);
            if (at(Tok::Ident)) {
                p->loc = cur().loc;
                p->name = advance().text;
            } else {
                error(cur().loc, "script.parse.parameter_name", "every parameter needs a name",
                      "name it, even when it goes unused: process(Block& io)");
            }
            if (atPunct("[")) declaratorInit(p->spec, p->name);
            if (accept("=")) p->init = assignment();
            f->params.push_back(p);
            if (!accept(",")) break;
        }
        expect(")", "to close the parameters");
        while (true) {
            if (acceptKeyword("const"))
                f->isConst = true;
            else if (acceptKeyword("noexcept")) {
                if (atPunct("(")) {
                    // C++ would evaluate the condition; the subset has
                    // nothing to say with it.
                    error(cur().loc, "script.unsupported.noexcept",
                          "noexcept with a condition is not in the subset",
                          "write noexcept alone, or nothing");
                    skipParens();
                }
            } else if (at(Tok::Ident) && (cur().text == "override" || cur().text == "final")) {
                error(cur().loc, "script.unsupported.virtual",
                      "virtual functions are not in the subset", "remove '" + cur().text + "'");
                advance();
            } else if (atPunct("->")) {
                error(cur().loc, "script.unsupported.trailing_return",
                      "trailing return types are not in the subset", "write the result type first");
                advance();
                typeSpec(false);
            } else
                break;
        }
        if (atPunct("=") &&
            (peek().keyword("default") || peek().keyword("delete") || peek().kind == Tok::IntLit)) {
            error(cur().loc, "script.unsupported.special",
                  "= default, = delete and pure virtual functions are not in the subset",
                  "remove the declaration");
            recover();
            return f;
        }
        if (accept(";")) {
            error(name.loc, "script.unsupported.declaration_only",
                  "'" + name.text + "' is declared without a body",
                  "define it here: a script has no separate declarations");
        } else {
            f->body = compound();
        }
        if (owner) owner->methods.push_back(f);
        return owner ? nullptr : f;
    }

    // --- statements ----------------------------------------------------------------

    CompoundStmt* compound() {
        auto* c = unit_.stmt<CompoundStmt>(cur().loc);
        expect("{", "to open the block");
        while (!atPunct("}") && !at(Tok::End)) {
            const size_t before = pos_;
            if (Stmt* s = statement()) c->body.push_back(s);
            if (pos_ == before) advance();
        }
        expect("}", "to close the block");
        return c;
    }

    Stmt* statement() {
        const Nest nest(*this);
        const Token& t = cur();
        const Loc loc = t.loc;
        if (atPunct("{")) return compound();
        if (accept(";")) return unit_.stmt<EmptyStmt>(loc);
        if (t.kind == Tok::Keyword) {
            const std::string& k = t.text;
            if (k == "if") return ifStmt();
            if (k == "for") return forStmt();
            if (k == "while") {
                advance();
                auto* w = unit_.stmt<WhileStmt>(loc);
                expect("(");
                w->cond = expression();
                expect(")");
                w->body = statement();
                return w;
            }
            if (k == "do") {
                advance();
                auto* d = unit_.stmt<DoWhileStmt>(loc);
                d->body = statement();
                if (!acceptKeyword("while"))
                    error(cur().loc, "script.parse.expected",
                          "expected 'while' after the do statement's body",
                          "write do { ... } while (condition);");
                expect("(");
                d->cond = expression();
                expect(")");
                expect(";");
                return d;
            }
            if (k == "switch") {
                advance();
                auto* s = unit_.stmt<SwitchStmt>(loc);
                expect("(");
                s->cond = expression();
                expect(")");
                if (!atPunct("{")) {
                    error(cur().loc, "script.parse.switch", "a switch's body must be a block",
                          "switch (x) { case 0: ... }");
                    return s;
                }
                s->body = compound();
                return s;
            }
            if (k == "case") {
                advance();
                auto* c = unit_.stmt<CaseStmt>(loc);
                c->value = conditional();
                expect(":", "after the case value");
                return c;
            }
            if (k == "default") {
                advance();
                auto* c = unit_.stmt<CaseStmt>(loc);
                expect(":", "after default");
                return c;
            }
            if (k == "break") {
                advance();
                expect(";");
                return unit_.stmt<BreakStmt>(loc);
            }
            if (k == "continue") {
                advance();
                expect(";");
                return unit_.stmt<ContinueStmt>(loc);
            }
            if (k == "return") {
                advance();
                auto* r = unit_.stmt<ReturnStmt>(loc);
                if (!atPunct(";")) r->value = atPunct("{") ? initList() : expression();
                expect(";", "after the return value");
                return r;
            }
            if (k == "goto") {
                error(loc, "script.unsupported.goto", "goto is not in the subset",
                      "use break, continue or a flag");
                recover();
                return nullptr;
            }
            if (k == "try" || k == "throw" || k == "catch") {
                error(loc, "script.unsupported.exceptions", "exceptions are not in the subset",
                      "check the condition and handle it in place");
                recover();
                return nullptr;
            }
            if (k == "static_assert" || k == "asm" || k == "typedef" || k == "using" ||
                k == "namespace" || k == "template") {
                error(loc, "script.unsupported.statement", "'" + k + "' is not in the subset",
                      "remove it");
                recover();
                return nullptr;
            }
            if (k == "struct" || k == "class" || k == "enum" || k == "union") {
                error(loc, "script.unsupported.local_type",
                      "types are declared at the top level or in a struct",
                      "move this declaration out of the function");
                recover();
                return nullptr;
            }
        }
        if (t.kind == Tok::Ident && peek().punct(":") && !peek().punct("::")) {
            error(loc, "script.unsupported.label", "labels are not in the subset",
                  "remove '" + t.text + ":'");
            advance();
            advance();
            return nullptr;
        }
        if (startsType()) return declStmt();
        auto* e = unit_.stmt<ExprStmt>(loc);
        e->expr = expression();
        expect(";", "after the expression");
        return e;
    }

    Stmt* declStmt() {
        auto* d = unit_.stmt<DeclStmt>(cur().loc);
        TypeSpec* type = typeSpec(true);
        if (type->isStatic && !type->isConstexpr)
            error(type->loc, "script.unsupported.static_local",
                  "a static local would be shared by every instance of the plugin",
                  "make it a member of the plugin struct");
        while (true) {
            if (!at(Tok::Ident)) {
                error(cur().loc, "script.parse.name",
                      "expected a variable name before " + describe(cur()),
                      "write the type, then the name: double y = 0.0;");
                recover();
                return d;
            }
            auto* v = unit_.decl<VarDecl>(cur().loc);
            v->name = advance().text;
            v->spec = type;
            v->init = declaratorInit(type, v->name);
            d->vars.push_back(v);
            if (!accept(",")) break;
        }
        expect(";", "after the declaration");
        return d;
    }

    Stmt* ifStmt() {
        auto* s = unit_.stmt<IfStmt>(advance().loc);
        if (acceptKeyword("constexpr")) s->isConstexpr = true;
        expect("(");
        if (startsType()) {
            error(cur().loc, "script.unsupported.if_init",
                  "declarations in an if's condition are not in the subset",
                  "declare the variable on the line before");
            typeSpec(false);
        }
        s->cond = expression();
        if (accept(";")) {
            error(cur().loc, "script.unsupported.if_init",
                  "an if with an initializer is not in the subset",
                  "declare the variable on the line before");
            s->cond = expression();
        }
        expect(")");
        s->then = statement();
        if (acceptKeyword("else")) s->otherwise = statement();
        return s;
    }

    Stmt* forStmt() {
        const Loc loc = advance().loc;
        expect("(");
        // A range-based for: for (auto& v : voices).
        if (startsType()) {
            const size_t save = pos_;
            TypeSpec* type = typeSpec(false);
            if (at(Tok::Ident) && peek().punct(":")) {
                auto* r = unit_.stmt<RangeForStmt>(loc);
                r->var = unit_.decl<VarDecl>(cur().loc);
                r->var->name = advance().text;
                r->var->spec = type;
                advance();  // ':'
                r->range = expression();
                expect(")");
                r->body = statement();
                return r;
            }
            pos_ = save;
        }
        auto* f = unit_.stmt<ForStmt>(loc);
        if (!accept(";")) {
            if (startsType()) {
                f->init = declStmt();
            } else {
                auto* e = unit_.stmt<ExprStmt>(cur().loc);
                e->expr = expression();
                expect(";");
                f->init = e;
            }
        }
        if (!atPunct(";")) f->cond = expression();
        expect(";");
        if (!atPunct(")")) f->step = expression();
        expect(")");
        f->body = statement();
        return f;
    }

    // --- expressions -----------------------------------------------------------------

    Expr* expression() {
        Expr* e = assignment();
        int links = 0;
        while (atPunct(",")) {
            deeper();
            ++links;
            auto* b = unit_.expr<BinaryExpr>(advance().loc);
            b->op = ",";
            b->left = e;
            b->right = assignment();
            e = b;
        }
        depth_ -= links;
        return e;
    }

    Expr* assignment() {
        const Nest nest(*this);
        Expr* left = conditional();
        static const char* ops[] = {
            "=", "+=", "-=", "*=", "/=", "%=", "<<=", ">>=", "&=", "|=", "^="};
        for (const char* op : ops)
            if (atPunct(op)) {
                auto* a = unit_.expr<AssignExpr>(advance().loc);
                a->op = op;
                a->target = left;
                a->value = atPunct("{") ? initList() : assignment();
                return a;
            }
        return left;
    }

    Expr* conditional() {
        Expr* c = binary(0);
        if (atPunct("?")) {
            auto* e = unit_.expr<ConditionalExpr>(advance().loc);
            e->cond = c;
            e->then = expression();
            expect(":", "in the conditional expression");
            e->otherwise = assignment();
            return e;
        }
        return c;
    }

    // Binary operators by C++ precedence, lowest first.
    static int precedence(const std::string& op) {
        if (op == "||") return 1;
        if (op == "&&") return 2;
        if (op == "|") return 3;
        if (op == "^") return 4;
        if (op == "&") return 5;
        if (op == "==" || op == "!=") return 6;
        if (op == "<" || op == ">" || op == "<=" || op == ">=" || op == "<=>") return 7;
        if (op == "<<" || op == ">>") return 8;
        if (op == "+" || op == "-") return 9;
        if (op == "*" || op == "/" || op == "%") return 10;
        return -1;
    }

    Expr* binary(int minPrec) {
        Expr* left = unary();
        int links = 0;
        while (true) {
            const Token& t = cur();
            if (t.kind != Tok::Punct) break;
            // Inside template arguments a '>' closes the list.
            if (inTemplate_ > 0 &&
                (t.text == ">" || t.text == ">>" || t.text == ">=" || t.text == ">>="))
                break;
            const int p = precedence(t.text);
            if (p < 0 || p < minPrec) break;
            const std::string op = t.text;
            const Loc loc = advance().loc;
            if (op == "<=>")
                error(loc, "script.unsupported.spaceship",
                      "the three-way comparison is not in the subset", "compare with <, == and >");
            deeper();
            ++links;
            Expr* right = binary(p + 1);
            auto* b = unit_.expr<BinaryExpr>(loc);
            b->op = op;
            b->left = left;
            b->right = right;
            left = b;
        }
        depth_ -= links;
        return left;
    }

    Expr* unary() {
        const Nest nest(*this);
        const Token& t = cur();
        if (t.kind == Tok::Punct) {
            if (t.text == "-" || t.text == "+" || t.text == "!" || t.text == "~" ||
                t.text == "++" || t.text == "--") {
                auto* u = unit_.expr<UnaryExpr>(advance().loc);
                u->op = t.text;
                u->operand = unary();
                return u;
            }
            if (t.text == "&") {
                error(t.loc, "script.unsupported.address", "taking an address is not in the subset",
                      "bind a reference instead: auto& v = voices[i];");
                advance();
                return unary();
            }
            if (t.text == "*") {
                error(t.loc, "script.unsupported.pointer", "pointers are not in the subset",
                      "use the object, or a reference to it");
                advance();
                return unary();
            }
            // A C-style cast: (double)x.
            if (t.text == "(" && isCastAhead()) {
                auto* c = unit_.expr<CastExpr>(advance().loc);
                c->cast = CastKind::CStyle;
                c->target = typeSpec(false);
                expect(")");
                c->operand = unary();
                return c;
            }
        }
        if (t.keyword("sizeof") || t.keyword("alignof") || t.keyword("typeid") ||
            t.keyword("decltype")) {
            error(t.loc, "script.unsupported.sizeof", "'" + t.text + "' is not in the subset",
                  "an Array knows its size: arr.size()");
            advance();
            if (atPunct("(")) skipParens();
            return unit_.expr<IntLitExpr>(t.loc);
        }
        if (t.keyword("new") || t.keyword("delete")) {
            error(t.loc, "script.unsupported.allocation", "dynamic allocation is not in the subset",
                  "declare a member instead (Array<float, 4096> for a fixed size, or a "
                  "Buffer<float> sized in "
                  "prepare())");
            advance();
            return unary();
        }
        if (t.keyword("co_await") || t.keyword("co_yield")) {
            error(t.loc, "script.unsupported.coroutine", "coroutines are not in the subset",
                  "write the steps as ordinary code that runs in process()");
            advance();
            return unary();
        }
        return postfix(primary());
    }

    // After '(': does a type and ')' follow, then something a cast applies to?
    bool isCastAhead() const {
        size_t k = 1;
        const Token& first = peek(k);
        if (!(isTypeKeyword(first) || first.keyword("const") ||
              (first.kind == Tok::Ident &&
               (types_.count(first.text) || isRuntimeType(first.text)))))
            return false;
        if (first.text == "auto") return false;
        // Walk the type: names, ::, template args, const, &, *.
        int angle = 0;
        while (true) {
            const Token& t = peek(k);
            if (t.kind == Tok::End) return false;
            if (t.punct("<"))
                ++angle;
            else if (t.punct(">"))
                --angle;
            else if (t.punct(">>"))
                angle -= 2;
            else if (t.punct(")") && angle <= 0)
                break;
            else if (angle == 0 &&
                     !(t.kind == Tok::Ident || isTypeKeyword(t) || t.keyword("const") ||
                       t.punct("::") || t.punct("&") || t.punct("*")))
                return false;
            ++k;
        }
        const Token& after = peek(k + 1);
        // (Type) followed by something that can start an operand.
        return !(after.kind == Tok::Punct &&
                 !(after.text == "(" || after.text == "-" || after.text == "+" ||
                   after.text == "!" || after.text == "~"));
    }

    void skipParens() {
        int depth = 0;
        do {
            if (atPunct("(")) ++depth;
            if (atPunct(")")) --depth;
            advance();
        } while (depth > 0 && !at(Tok::End));
    }

    Expr* primary() {
        const Token& t = cur();
        const Loc loc = t.loc;
        switch (t.kind) {
            case Tok::IntLit: {
                auto* e = unit_.expr<IntLitExpr>(loc);
                e->value = advance().intValue;
                return e;
            }
            case Tok::FloatLit: {
                auto* e = unit_.expr<FloatLitExpr>(loc);
                e->value = t.floatValue;
                e->isFloat = t.floatSuffix;
                advance();
                return e;
            }
            case Tok::StringLit: {
                auto* e = unit_.expr<StringLitExpr>(loc);
                while (at(Tok::StringLit)) e->value += advance().stringValue;  // "a" "b"
                return e;
            }
            case Tok::CharLit:
                error(loc, "script.unsupported.char", "character literals are not in the subset",
                      "use a string literal, or the character's number as an int");
                advance();
                return unit_.expr<IntLitExpr>(loc);
            case Tok::Keyword: {
                if (t.text == "true" || t.text == "false") {
                    auto* e = unit_.expr<BoolLitExpr>(loc);
                    e->value = advance().text == "true";
                    return e;
                }
                if (t.text == "nullptr") {
                    advance();
                    return unit_.expr<NullLitExpr>(loc);
                }
                if (t.text == "this") {
                    advance();
                    if (atPunct("->")) {
                        // this->member: allowed, as C++ spells it.
                        auto* m = unit_.expr<MemberExpr>(advance().loc);
                        m->object = unit_.expr<ThisExpr>(loc);
                        if (at(Tok::Ident)) m->member = advance().text;
                        return m;
                    }
                    error(loc, "script.unsupported.this", "'this' is only usable as this->member",
                          "name the member directly");
                    return unit_.expr<ThisExpr>(loc);
                }
                if (t.text == "static_cast" || t.text == "reinterpret_cast" ||
                    t.text == "const_cast" || t.text == "dynamic_cast") {
                    advance();
                    auto* c = unit_.expr<CastExpr>(loc);
                    c->cast = CastKind::Static;
                    if (t.text != "static_cast")
                        error(loc, "script.unsupported.cast",
                              "'" + t.text + "' is not in the subset",
                              "static_cast converts between numbers");
                    expect("<");
                    c->target = typeSpec(false);
                    expect(">");
                    expect("(");
                    c->operand = expression();
                    expect(")");
                    return c;
                }
                if (isTypeKeyword(t) && t.text != "auto" && t.text != "void") {
                    // A functional cast: double(x), float{x}.
                    auto* c = unit_.expr<CastExpr>(loc);
                    c->cast = CastKind::Functional;
                    c->target = typeSpec(false);
                    if (accept("(")) {
                        c->operand = expression();
                        expect(")");
                    } else if (atPunct("{")) {
                        auto* list = initList();
                        c->operand = list;
                    } else {
                        error(cur().loc, "script.parse.expected",
                              "expected '(' after the type name",
                              "convert with parentheses: double(n), or static_cast<double>(n)");
                    }
                    return c;
                }
                if (t.text == "operator" || t.text == "template" || t.text == "typename") {
                    error(loc, "script.unsupported.expression",
                          "'" + t.text + "' is not in the subset",
                          "write a named function; the subset has no templates or operator "
                          "overloading");
                    advance();
                    return unit_.expr<IntLitExpr>(loc);
                }
                break;
            }
            case Tok::Ident:
                return nameExpr();
            case Tok::Punct:
                if (t.text == "(") {
                    advance();
                    Expr* e = expression();
                    expect(")");
                    return e;
                }
                if (t.text == "{") return initList();
                if (t.text == "[") {
                    error(loc, "script.unsupported.lambda", "lambdas are not in the subset",
                          "write a member function and call it");
                    skipLambda();
                    return unit_.expr<IntLitExpr>(loc);
                }
                if (t.text == "::") {
                    advance();
                    return nameExpr();
                }
                break;
            default:
                break;
        }
        error(loc, "script.parse.expression", "expected an expression before " + describe(t),
              "put a value, a name or a call here; a missing operand or an extra operator is the "
              "usual "
              "cause");
        if (!at(Tok::End) && !atPunct(";") && !atPunct("}") && !atPunct(")")) advance();
        return unit_.expr<IntLitExpr>(loc);
    }

    void skipLambda() {
        while (!atPunct("{") && !at(Tok::End) && !atPunct(";")) advance();
        if (atPunct("{")) {
            int depth = 0;
            do {
                if (atPunct("{")) ++depth;
                if (atPunct("}")) --depth;
                advance();
            } while (depth > 0 && !at(Tok::End));
        }
    }

    Expr* nameExpr() {
        auto* n = unit_.expr<NameExpr>(cur().loc);
        n->path.push_back(advance().text);
        if (n->path[0] == "std" && atPunct("::")) {
            advance();
            const std::string name = at(Tok::Ident) ? advance().text : std::string();
            error(n->loc, "script.unsupported.std",
                  "std::" + name + " is not available in a script",
                  name.empty() ? ""
                               : "write " + name +
                                     "(...) without std::: the runtime has its own (see the "
                                     "language reference)");
            n->path = {name.empty() ? "std" : name};
            if (atPunct("<")) skipTemplateArgs();
            return n;
        }
        if (atPunct("<") && runtimeTemplates().count(n->path[0])) {
            // Array<float, 4>(...) as a value.
            auto* tn = unit_.expr<TypeNameExpr>(n->loc);
            tn->spec = unit_.spec();
            tn->spec->loc = n->loc;
            tn->spec->path = n->path;
            templateArgs(tn->spec);
            return tn;
        }
        while (atPunct("::") && peek().kind == Tok::Ident) {
            advance();
            n->path.push_back(advance().text);
        }
        return n;
    }

    Expr* postfix(Expr* e) {
        int links = 0;
        while (true) {
            const Token& t = cur();
            if (!(t.punct("(") || t.punct("[") || t.punct(".") || t.punct("->") || t.punct("++") ||
                  t.punct("--") || (t.punct("{") && e->kind == ExprKind::Name)))
                break;
            deeper();
            ++links;
            if (t.punct("(")) {
                auto* c = unit_.expr<CallExpr>(advance().loc);
                c->callee = e;
                while (!atPunct(")") && !at(Tok::End)) {
                    c->args.push_back(atPunct("{") ? initList() : assignment());
                    if (!accept(",")) break;
                }
                expect(")", "to close the call");
                e = c;
            } else if (t.punct("[")) {
                auto* i = unit_.expr<IndexExpr>(advance().loc);
                i->object = e;
                i->index = expression();
                expect("]");
                e = i;
            } else if (t.punct(".") || t.punct("->")) {
                const bool arrow = t.text == "->";
                auto* m = unit_.expr<MemberExpr>(advance().loc);
                m->object = e;
                m->arrow = arrow;
                if (arrow && e->kind != ExprKind::This)
                    error(m->loc, "script.unsupported.pointer",
                          "'->' needs a pointer, which the subset has none of", "write '.'");
                if (at(Tok::Ident))
                    m->member = advance().text;
                else
                    error(cur().loc, "script.parse.member",
                          "expected a member name after '" + t.text + "'",
                          "write the member's name after it: io.frames");
                e = m;
            } else if (t.punct("++") || t.punct("--")) {
                auto* u = unit_.expr<UnaryExpr>(advance().loc);
                u->op = t.text;
                u->postfix = true;
                u->operand = e;
                e = u;
            } else if (t.punct("{") && e->kind == ExprKind::Name) {
                // Type{...}: a struct value built in place.
                auto* c = unit_.expr<CastExpr>(t.loc);
                c->cast = CastKind::Functional;
                c->target = unit_.spec();
                c->target->loc = e->loc;
                c->target->path = static_cast<NameExpr*>(e)->path;
                c->operand = initList();
                e = c;
            }
        }
        depth_ -= links;
        return e;
    }

    InitListExpr* initList() {
        const Nest nest(*this);
        auto* l = unit_.expr<InitListExpr>(cur().loc);
        expect("{");
        while (!atPunct("}") && !at(Tok::End)) {
            InitListExpr::Item item;
            item.loc = cur().loc;
            if (atPunct(".") && peek().kind == Tok::Ident) {
                advance();
                item.designator = advance().text;
                if (atPunct("{")) {
                    item.value = initList();
                } else {
                    expect("=", "after the member's name");
                    item.value = atPunct("{") ? initList() : assignment();
                }
            } else {
                item.value = atPunct("{") ? initList() : assignment();
            }
            l->items.push_back(item);
            if (!accept(",")) break;
        }
        expect("}", "to close the braces");
        return l;
    }

    std::vector<Token>& toks_;
    Unit& unit_;
    Diagnostics& diags_;
    size_t pos_ = 0;
    int inTemplate_ = 0;
    int depth_ = 0;
    bool abandoned_ = false;
    std::set<std::string> types_;
};

}  // namespace

void parse(const std::vector<Token>& tokens, Unit& unit, Diagnostics& diags) {
    // The parser may turn a '>>' into '>' (closing nested templates), so it
    // works on its own copy.
    std::vector<Token> copy = tokens;
    Parser(copy, unit, diags).run();
}

}  // namespace supple::scriptc
