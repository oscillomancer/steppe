// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The script compiler's data: source positions and diagnostics, tokens, types
// and the syntax tree. The parser builds the tree; semantic analysis fills in
// each expression's type and what each name refers to, and inserts the
// implicit conversions C++ would make, so code generation reads a fully
// explicit tree.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "supple/script_program.hpp"

namespace supple::scriptc {

// One function's frame: declared locals and the compiler's temporaries.
inline constexpr int kMaxFrameBytes = 64 << 10;
// The frames along any chain of calls, which an audio thread's stack holds.
inline constexpr int kMaxStackBytes = 256 << 10;

// --- positions and diagnostics -------------------------------------------------

struct Loc {
    int line = 0;    // 1-based; 0 for "nowhere in the source"
    int column = 0;  // 1-based, in bytes
};

struct Diagnostic {
    enum class Severity { Error, Warning };
    Severity severity = Severity::Error;
    std::string code;  // "script.<area>.<what>", stable for tools
    std::string message;
    std::string hint;  // how to fix it, in the subset's terms
    Loc loc;
};

class Diagnostics {
  public:
    void error(Loc loc, std::string code, std::string message, std::string hint = {});
    void warning(Loc loc, std::string code, std::string message, std::string hint = {});
    bool hasErrors() const { return errors_ > 0; }
    const std::vector<Diagnostic>& list() const { return list_; }
    // Stops reporting after this many errors: past it they are mostly echoes.
    static constexpr int kMaxErrors = 50;

  private:
    std::vector<Diagnostic> list_;
    int errors_ = 0;
};

// --- tokens ----------------------------------------------------------------------

enum class Tok : std::uint8_t {
    End,
    Ident,
    Keyword,
    IntLit,
    FloatLit,
    StringLit,
    CharLit,
    Punct,
    Hash,  // a preprocessor line, its text in `text`
};

struct Token {
    Tok kind = Tok::End;
    std::string text;  // identifier, keyword, punctuator, literal spelling
    Loc loc;
    // Literals.
    std::int64_t intValue = 0;
    double floatValue = 0.0;
    bool floatSuffix = false;  // 1.0f
    std::string stringValue;   // decoded

    bool is(Tok k, std::string_view t) const { return kind == k && text == t; }
    bool punct(std::string_view t) const { return kind == Tok::Punct && text == t; }
    bool keyword(std::string_view t) const { return kind == Tok::Keyword && text == t; }
};

std::vector<Token> lex(std::string_view source, Diagnostics& diags);

// --- types -----------------------------------------------------------------------

struct StructDecl;
struct EnumDecl;
struct BuiltinClass;

enum class TypeKind : std::uint8_t {
    Void,
    Bool,
    Int,
    Float,
    Double,
    Enum,      // a scoped enum: the script's own or the runtime's
    Struct,    // a struct the script declares
    Array,     // Array<T, N>
    Buffer,    // Buffer<T>
    Object,    // a runtime class: Biquad, DelayLine, Block, Param, ...
    String,    // a string literal (const char*): only where the runtime takes text
    InitList,  // a braced list, before it meets the type it initializes
    Null,      // nullptr, which nothing accepts
};

struct Type {
    TypeKind kind = TypeKind::Void;
    std::string name;  // as the script spells it: "double", "Array<float, 16>", "Biquad"
    int size = 0;
    int align = 1;
    const Type* element = nullptr;  // Array, Buffer
    int count = 0;                  // Array
    StructDecl* structDecl = nullptr;
    EnumDecl* enumDecl = nullptr;
    const BuiltinClass* builtin = nullptr;
    bool shared = false;  // a SharedArray<T, N>: an Array the editor sees

    bool isArithmetic() const {
        return kind == TypeKind::Bool || kind == TypeKind::Int || kind == TypeKind::Float ||
               kind == TypeKind::Double;
    }
    bool isFloating() const { return kind == TypeKind::Float || kind == TypeKind::Double; }
    bool isIntegral() const { return kind == TypeKind::Int || kind == TypeKind::Bool; }
    bool isScalar() const { return isArithmetic() || kind == TypeKind::Enum; }
    bool isVoid() const { return kind == TypeKind::Void; }
};

// A declared type: what a variable, parameter or result holds.
struct QualType {
    const Type* type = nullptr;
    bool isConst = false;
    bool isRef = false;
    bool operator==(const QualType&) const = default;
};

// --- the syntax tree ---------------------------------------------------------------

struct Decl;
struct VarDecl;
struct FunctionDecl;
struct FieldDecl;
struct BuiltinFunction;
struct BuiltinMethod;
struct Stmt;

// How an expression's value is used later: sema fills these in.
enum class ExprKind : std::uint8_t {
    IntLit,
    FloatLit,
    BoolLit,
    StringLit,
    NullLit,
    Name,         // an identifier, possibly qualified (a::b)
    Member,       // object.member
    Index,        // object[index]
    Call,         // callee(args)
    Unary,        // -x, !x, ~x, ++x, --x, x++, x--
    Binary,       // x + y, x && y, ...
    Assign,       // x = y, x += y, ...
    Conditional,  // c ? a : b
    Cast,         // T(x), static_cast<T>(x), (T)x, and the implicit ones sema inserts
    InitList,     // { ... } and { .field = ... }
    This,
    TypeName,  // a type used where an expression is parsed: Array<float, 4>{} is not allowed, but
               // Oscillator::Shape is
};

// What a name or member turned out to be.
enum class RefKind : std::uint8_t {
    None,
    Local,          // a local variable or parameter
    Global,         // a namespace-scope constant
    Field,          // a data member of a script struct (through `object`)
    StaticConst,    // a static constexpr member
    Function,       // a free function of the script
    Method,         // a member function of a script struct
    Builtin,        // a runtime function
    BuiltinMethod,  // a runtime member function
    BuiltinField,   // a field of a runtime class (io.frames, transport.tempo)
    Enumerator,
    Type,
};

enum class CastKind : std::uint8_t {
    None,
    Implicit,    // inserted by sema
    Functional,  // T(x)
    Static,      // static_cast<T>(x)
    CStyle,      // (T)x
};

struct Expr {
    ExprKind kind;
    Loc loc;
    explicit Expr(ExprKind k, Loc l) : kind(k), loc(l) {}
    virtual ~Expr() = default;

    // Filled in by sema.
    QualType qt;            // type and constness; isRef means "an lvalue"
    bool lvalue = false;    // designates an object (can be assigned or referenced)
    bool constant = false;  // a constant expression
    double constValue = 0.0;
    std::int64_t constInt = 0;

    const Type* type() const { return qt.type; }
};

struct IntLitExpr : Expr {
    std::int64_t value = 0;
    explicit IntLitExpr(Loc l) : Expr(ExprKind::IntLit, l) {}
};
struct FloatLitExpr : Expr {
    double value = 0.0;
    bool isFloat = false;  // the f suffix
    explicit FloatLitExpr(Loc l) : Expr(ExprKind::FloatLit, l) {}
};
struct BoolLitExpr : Expr {
    bool value = false;
    explicit BoolLitExpr(Loc l) : Expr(ExprKind::BoolLit, l) {}
};
struct StringLitExpr : Expr {
    std::string value;
    explicit StringLitExpr(Loc l) : Expr(ExprKind::StringLit, l) {}
};
struct NullLitExpr : Expr {
    explicit NullLitExpr(Loc l) : Expr(ExprKind::NullLit, l) {}
};

struct NameExpr : Expr {
    std::vector<std::string> path;               // {"Oscillator", "Shape", "Saw"}, {"x"}
    std::vector<struct TypeSpec*> templateArgs;  // a template-id used as a value (rare)
    // Sema.
    RefKind ref = RefKind::None;
    VarDecl* var = nullptr;
    FieldDecl* field = nullptr;
    FunctionDecl* function = nullptr;
    std::vector<const BuiltinFunction*> builtins;  // the overload set
    const BuiltinFunction* builtin = nullptr;      // the one chosen
    int enumValue = 0;
    const Type* namedType = nullptr;  // RefKind::Type
    explicit NameExpr(Loc l) : Expr(ExprKind::Name, l) {}
};

struct MemberExpr : Expr {
    Expr* object = nullptr;
    std::string member;
    bool arrow = false;  // a->b, which the subset refuses
    // Sema.
    RefKind ref = RefKind::None;
    FieldDecl* field = nullptr;
    FunctionDecl* method = nullptr;
    std::vector<const BuiltinMethod*> builtinMethods;
    const BuiltinMethod* builtinMethod = nullptr;
    int builtinField = -1;  // which field of a runtime class
    explicit MemberExpr(Loc l) : Expr(ExprKind::Member, l) {}
};

struct IndexExpr : Expr {
    Expr* object = nullptr;
    Expr* index = nullptr;
    explicit IndexExpr(Loc l) : Expr(ExprKind::Index, l) {}
};

struct CallExpr : Expr {
    Expr* callee = nullptr;
    std::vector<Expr*> args;
    // Sema: what is called, and how each argument is passed.
    FunctionDecl* function = nullptr;              // a script function or method
    const BuiltinFunction* builtin = nullptr;      // a runtime function
    const BuiltinMethod* builtinMethod = nullptr;  // a runtime method
    Expr* object = nullptr;                        // the object a method is called on (null: this)
    bool implicitThis = false;
    explicit CallExpr(Loc l) : Expr(ExprKind::Call, l) {}
};

struct UnaryExpr : Expr {
    std::string op;  // "-", "+", "!", "~", "++", "--"
    bool postfix = false;
    Expr* operand = nullptr;
    explicit UnaryExpr(Loc l) : Expr(ExprKind::Unary, l) {}
};

struct BinaryExpr : Expr {
    std::string op;  // "+", "-", "*", "/", "%", "<<", ">>", "<", "<=", ">", ">=", "==", "!=", "&",
                     // "^", "|", "&&", "||", ","
    Expr* left = nullptr;
    Expr* right = nullptr;
    // Sema: the type both operands were converted to (the usual arithmetic
    // conversions), which the operation is performed in.
    const Type* operandType = nullptr;
    explicit BinaryExpr(Loc l) : Expr(ExprKind::Binary, l) {}
};

struct AssignExpr : Expr {
    std::string op;  // "=", "+=", ...
    Expr* target = nullptr;
    Expr* value = nullptr;
    const Type* operandType = nullptr;  // compound: the type the operation is done in
    explicit AssignExpr(Loc l) : Expr(ExprKind::Assign, l) {}
};

struct ConditionalExpr : Expr {
    Expr* cond = nullptr;
    Expr* then = nullptr;
    Expr* otherwise = nullptr;
    explicit ConditionalExpr(Loc l) : Expr(ExprKind::Conditional, l) {}
};

struct TypeSpec;

struct CastExpr : Expr {
    CastKind cast = CastKind::Implicit;
    TypeSpec* target = nullptr;  // as written; null for an implicit cast
    Expr* operand = nullptr;
    bool userConversion = false;  // Param to double, through its operator double()
    explicit CastExpr(Loc l) : Expr(ExprKind::Cast, l) {}
};

struct InitListExpr : Expr {
    struct Item {
        std::string designator;  // .name = value; empty for positional
        Loc loc;
        Expr* value = nullptr;
    };
    std::vector<Item> items;
    explicit InitListExpr(Loc l) : Expr(ExprKind::InitList, l) {}
};

struct ThisExpr : Expr {
    explicit ThisExpr(Loc l) : Expr(ExprKind::This, l) {}
};

struct TypeNameExpr : Expr {
    TypeSpec* spec = nullptr;
    explicit TypeNameExpr(Loc l) : Expr(ExprKind::TypeName, l) {}
};

// A type as written: `const double&`, `Array<Voice, 8>`, `auto&`.
struct TypeSpec {
    Loc loc;
    std::vector<std::string> path;    // {"double"}, {"Oscillator", "Shape"}
    std::vector<TypeSpec*> typeArgs;  // template arguments that are types
    std::vector<Expr*> valueArgs;     // template arguments that are values (N)
    std::vector<bool> argIsType;      // in order: which of the two lists each argument came from
    bool isConst = false;
    bool isConstexpr = false;
    bool isStatic = false;
    bool isRef = false;
    bool isAuto = false;
    int pointers = 0;          // T* (refused)
    bool rvalueRef = false;    // T&& (refused)
    bool unsupported = false;  // unsigned, long, char...: already reported
    // Sema.
    const Type* resolved = nullptr;
};

// --- statements ----------------------------------------------------------------

enum class StmtKind : std::uint8_t {
    Compound,
    Expr,
    Decl,
    If,
    For,
    RangeFor,
    While,
    DoWhile,
    Switch,
    Case,
    Break,
    Continue,
    Return,
    Empty,
};

struct Stmt {
    StmtKind kind;
    Loc loc;
    explicit Stmt(StmtKind k, Loc l) : kind(k), loc(l) {}
    virtual ~Stmt() = default;
};

struct CompoundStmt : Stmt {
    std::vector<Stmt*> body;
    explicit CompoundStmt(Loc l) : Stmt(StmtKind::Compound, l) {}
};
struct ExprStmt : Stmt {
    Expr* expr = nullptr;
    explicit ExprStmt(Loc l) : Stmt(StmtKind::Expr, l) {}
};
struct DeclStmt : Stmt {
    std::vector<VarDecl*> vars;
    explicit DeclStmt(Loc l) : Stmt(StmtKind::Decl, l) {}
};
struct IfStmt : Stmt {
    Expr* cond = nullptr;
    Stmt* then = nullptr;
    Stmt* otherwise = nullptr;
    bool isConstexpr = false;
    explicit IfStmt(Loc l) : Stmt(StmtKind::If, l) {}
};
struct ForStmt : Stmt {
    Stmt* init = nullptr;  // a DeclStmt or ExprStmt, or null
    Expr* cond = nullptr;
    Expr* step = nullptr;
    Stmt* body = nullptr;
    explicit ForStmt(Loc l) : Stmt(StmtKind::For, l) {}
};
struct RangeForStmt : Stmt {
    VarDecl* var = nullptr;
    Expr* range = nullptr;
    Stmt* body = nullptr;
    explicit RangeForStmt(Loc l) : Stmt(StmtKind::RangeFor, l) {}
};
struct WhileStmt : Stmt {
    Expr* cond = nullptr;
    Stmt* body = nullptr;
    explicit WhileStmt(Loc l) : Stmt(StmtKind::While, l) {}
};
struct DoWhileStmt : Stmt {
    Stmt* body = nullptr;
    Expr* cond = nullptr;
    explicit DoWhileStmt(Loc l) : Stmt(StmtKind::DoWhile, l) {}
};
struct CaseStmt : Stmt {
    Expr* value = nullptr;  // null: default
    std::int64_t constValue = 0;
    explicit CaseStmt(Loc l) : Stmt(StmtKind::Case, l) {}
};
struct SwitchStmt : Stmt {
    Expr* cond = nullptr;
    CompoundStmt* body = nullptr;
    explicit SwitchStmt(Loc l) : Stmt(StmtKind::Switch, l) {}
};
struct BreakStmt : Stmt {
    explicit BreakStmt(Loc l) : Stmt(StmtKind::Break, l) {}
};
struct ContinueStmt : Stmt {
    explicit ContinueStmt(Loc l) : Stmt(StmtKind::Continue, l) {}
};
struct ReturnStmt : Stmt {
    Expr* value = nullptr;
    explicit ReturnStmt(Loc l) : Stmt(StmtKind::Return, l) {}
};
struct EmptyStmt : Stmt {
    explicit EmptyStmt(Loc l) : Stmt(StmtKind::Empty, l) {}
};

// --- declarations ----------------------------------------------------------------

enum class DeclKind : std::uint8_t { Var, Field, Function, Struct, Enum, Using, Include };

struct Decl {
    DeclKind kind;
    Loc loc;
    std::string name;
    explicit Decl(DeclKind k, Loc l) : kind(k), loc(l) {}
    virtual ~Decl() = default;
};

// A variable: local, parameter, or namespace-scope constant.
struct VarDecl : Decl {
    TypeSpec* spec = nullptr;
    Expr* init = nullptr;
    bool isParam = false;
    bool isGlobal = false;
    // Sema.
    QualType qt;
    bool addressTaken = false;  // bound to a reference, or a struct/array: lives in memory
    int id = -1;                // per function, for code generation
    explicit VarDecl(Loc l) : Decl(DeclKind::Var, l) {}
};

struct FieldDecl : Decl {
    TypeSpec* spec = nullptr;
    Expr* init = nullptr;  // the default member initializer
    bool isStatic = false;
    StructDecl* owner = nullptr;
    // Sema.
    QualType qt;
    int offset = 0;
    int paramIndex = -1;  // a Param member: its place in the parameter table
    bool oneDeclarator = true;
    int sourceIndex = -1;  // a Source member: its place among the sources
    explicit FieldDecl(Loc l) : Decl(DeclKind::Field, l) {}
};

// Where a function runs, which decides the rules it is held to.
enum Context : std::uint8_t {
    kInit = 1u << 0,   // prepare(), member initializers: may allocate
    kAudio = 1u << 1,  // process(), update() and the note hooks: real-time
    kUi = 1u << 2,     // the editor: may format text
};

struct FunctionDecl : Decl {
    TypeSpec* result = nullptr;
    std::vector<VarDecl*> params;
    CompoundStmt* body = nullptr;  // null: declared only (refused)
    StructDecl* owner = nullptr;   // a member function's struct
    bool isConst = false;          // void f() const
    bool isStatic = false;
    bool isConstexpr = false;
    // Sema.
    QualType resultType;
    std::uint8_t contexts = 0;  // where it is called from
    std::vector<FunctionDecl*> calls;
    std::vector<VarDecl*> locals;  // every local, parameters first
    bool analysed = false;
    // Effects, for the evaluation-order rule: which of `this`'s members and
    // which reference parameters it may write.
    bool writesThis = false;
    std::vector<int> writesParams;
    bool readsThis = true;
    explicit FunctionDecl(Loc l) : Decl(DeclKind::Function, l) {}
};

struct StructDecl : Decl {
    std::vector<FieldDecl*> fields;  // data members and static constants, in order
    std::vector<FunctionDecl*> methods;
    std::vector<StructDecl*> nested;
    std::vector<EnumDecl*> enums;
    StructDecl* outer = nullptr;
    PluginKind base = PluginKind::None;  // derives from Effect, Instrument or NoteEffect
    std::string baseName;
    Loc baseLoc;
    bool isClass = false;
    // Sema.
    Type* type = nullptr;
    bool laidOut = false;
    bool layingOut = false;
    explicit StructDecl(Loc l) : Decl(DeclKind::Struct, l) {}
};

struct EnumDecl : Decl {
    std::vector<std::pair<std::string, Expr*>> enumerators;
    std::vector<int> values;
    bool scoped = true;
    StructDecl* outer = nullptr;
    Type* type = nullptr;
    explicit EnumDecl(Loc l) : Decl(DeclKind::Enum, l) {}
};

struct UsingDecl : Decl {
    std::vector<std::string> path;
    explicit UsingDecl(Loc l) : Decl(DeclKind::Using, l) {}
};

struct IncludeDecl : Decl {
    std::string header;
    bool angled = false;
    explicit IncludeDecl(Loc l) : Decl(DeclKind::Include, l) {}
};

// The parsed file, owning every node.
struct Unit {
    std::vector<Decl*> decls;
    std::vector<std::unique_ptr<Expr>> exprs;
    std::vector<std::unique_ptr<Stmt>> stmts;
    std::vector<std::unique_ptr<Decl>> declPool;
    std::vector<std::unique_ptr<TypeSpec>> specs;
    std::vector<std::unique_ptr<Type>> types;

    template <class T, class... A>
    T* expr(A&&... a) {
        auto p = std::make_unique<T>(std::forward<A>(a)...);
        T* raw = p.get();
        exprs.push_back(std::move(p));
        return raw;
    }
    template <class T, class... A>
    T* stmt(A&&... a) {
        auto p = std::make_unique<T>(std::forward<A>(a)...);
        T* raw = p.get();
        stmts.push_back(std::move(p));
        return raw;
    }
    template <class T, class... A>
    T* decl(A&&... a) {
        auto p = std::make_unique<T>(std::forward<A>(a)...);
        T* raw = p.get();
        declPool.push_back(std::move(p));
        return raw;
    }
    TypeSpec* spec() {
        specs.push_back(std::make_unique<TypeSpec>());
        return specs.back().get();
    }
};

void parse(const std::vector<Token>& tokens, Unit& unit, Diagnostics& diags);

}  // namespace supple::scriptc
