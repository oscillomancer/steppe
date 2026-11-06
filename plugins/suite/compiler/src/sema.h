// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Semantic analysis, internal. sema.cpp has declarations, types, layout,
// statements, the plugin and the whole-program checks; sema_expr.cpp has
// expressions, C++'s conversion and overload rules, and constant evaluation.
#pragma once

#include <map>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include "builtins.h"
#include "model.h"

namespace supple::scriptc {

// A value known at compile time.
struct Constant {
    enum class Kind { None, Bool, Int, Double, String, List, Spec, File, Source, Port, Enum };
    Kind kind = Kind::None;
    bool boolValue = false;
    std::int64_t intValue = 0;
    double doubleValue = 0.0;
    std::string stringValue;
    std::vector<Constant> list;  // a braced list: elements, or designated fields
    std::vector<std::string> designators;
    ParamModel spec;  // a ParamSpec under construction
    // Its .activeWhen()/.activeUnless()/.activeAbove()/.activeBelow()
    // conditions, by member name: resolved once every parameter is known
    // (Sema::resolveActivity).
    struct Condition {
        std::string member;
        std::vector<std::string> options;
        bool listed = false, unless = false;  // listed: options were given
        bool numeric = false, below = false;  // activeAbove (or Below) bound
        double bound = 0.0;
        Loc loc;
    };
    std::vector<Condition> conditions;
    FileModel file;  // a FileSpec under construction
    PortModel port;
    SourceModel source;  // a SourceSpec under construction
    const Type* enumType = nullptr;
    std::string builtinEnum;  // "Curve" or "Unit" for the runtime's enums (no enumType)

    bool numeric() const { return kind == Kind::Bool || kind == Kind::Int || kind == Kind::Double; }
    double asDouble() const {
        return kind == Kind::Double ? doubleValue
               : kind == Kind::Int  ? static_cast<double>(intValue)
               : boolValue          ? 1.0
                                    : 0.0;
    }
};

// One conversion's cost, for overload resolution ([over.ics.rank]).
struct ConversionRank {
    enum Rank { Exact = 0, Promotion = 1, Conversion = 2, NotViable = 9 };
    Rank rank = NotViable;
    bool user = false;  // through Param's operator double
    bool operator==(const ConversionRank&) const = default;
};

// Where the expression being checked sits.
struct ExprContext {
    bool inConstMethod = false;
};

// An access to storage, for the evaluation-order rule: which variable or
// member (and below it which members), and whether it writes.
struct Access {
    int root = 0;                   // >= 0: a local's id; -1: this; -2: anything (unknown)
    std::vector<const void*> path;  // FieldDecls; nullptr: any element
    bool write = false;
};

class Sema {
  public:
    Sema(Unit& unit, Analysis& out, Diagnostics& diags);
    void run();

    // --- shared with sema_expr.cpp ---
    Unit& unit_;
    Analysis& out_;
    Diagnostics& diags_;
    const Builtins& b_;

    // Declarations.
    std::map<std::string, StructDecl*> structs_;  // top-level and nested, by simple name
    std::map<std::string, EnumDecl*> enums_;
    std::map<std::string, FunctionDecl*> functions_;
    std::map<std::string, VarDecl*> globals_;
    std::vector<StructDecl*> allStructs_;
    std::vector<EnumDecl*> allEnums_;
    std::map<std::string, Type*> arrays_;  // interned Array<T, N>, Buffer<T>

    // The function being checked.
    FunctionDecl* fn_ = nullptr;
    StructDecl* self_ = nullptr;  // the struct whose member function it is
    std::vector<std::map<std::string, VarDecl*>> scopes_;
    // The local whose initializer is being checked: in C++ its name already
    // means the new variable there.
    const VarDecl* declaring_ = nullptr;
    int loopDepth_ = 0, switchDepth_ = 0;
    bool inCallee_ = false;  // checking a call's callee: a function name is expected there
    std::vector<std::vector<CaseStmt*>> switchCases_;

    void error(Loc loc, const std::string& code, const std::string& message,
               const std::string& hint = {}) {
        diags_.error(loc, code, message, hint);
    }

    // Types.
    const Type* resolve(TypeSpec* spec, StructDecl* scope, bool allowVoid = false);
    const Type* arrayType(const Type* element, int count, bool shared = false,
                          const std::string& members = {});
    const Type* bufferType(const Type* element);
    StructDecl* findStruct(const std::string& name, StructDecl* scope) const;
    EnumDecl* findEnum(const std::string& name, StructDecl* scope) const;
    void layout(StructDecl* s);
    bool copyable(const Type* t) const;    // no resource inside
    bool declarable(const Type* t) const;  // may be a local
    std::string typeName(const QualType& q) const;
    static bool isPlain(const Type* t) { return t && t->isScalar(); }

    // Expressions (sema_expr.cpp).
    Expr* check(Expr* e);
    Expr* rvalue(Expr* e);  // checked, then read: a value of scalar type
    Expr* convert(Expr* e, QualType to, Loc loc, const std::string& what, bool braced = false);
    Expr* condition(Expr* e);
    Expr* implicitCast(Expr* e, const Type* to, bool user = false);
    ConversionRank rank(const Expr* from, QualType to) const;
    ConversionRank standardRank(const Type* from, const Type* to) const;
    bool better(const std::vector<ConversionRank>& a, const std::vector<ConversionRank>& b) const;
    const Type* promoted(const Type* t) const;
    const Type* arithmeticResult(const Type* a, const Type* b) const;
    Expr* checkName(NameExpr* n);
    Expr* checkMember(MemberExpr* m);
    Expr* checkIndex(IndexExpr* x);
    Expr* checkCall(CallExpr* c);
    void checkFormat(CallExpr* c);
    void checkTrace(CallExpr* c);
    std::set<std::string> traceNames_;  // the names given to trace() so far
    Expr* checkUnary(UnaryExpr* u);
    Expr* checkBinary(BinaryExpr* bin);
    Expr* checkAssign(AssignExpr* a);
    Expr* checkConditional(ConditionalExpr* c);
    Expr* checkCast(CastExpr* c);
    bool requireModifiable(Expr* e, Loc loc, const std::string& what);
    VarDecl* lookupLocal(const std::string& name) const;
    void markAddressTaken(Expr* e);
    // C++ finds a free function or a constant only below its declaration.
    void declaredBefore(Loc declared, Loc use, const std::string& name);
    std::vector<Expr*> callArguments(CallExpr* c, const std::vector<QualType>& params,
                                     const std::vector<Expr*>& defaults, const std::string& what);

    // Constants.
    std::optional<Constant> evaluate(Expr* e, StructDecl* scope);
    bool constantInt(Expr* e, std::int64_t& out);

    // Statements and functions (sema.cpp).
    void checkStmt(Stmt* s);
    void checkBlock(CompoundStmt* c, bool newScope = true);
    void declareLocal(VarDecl* v);
    void checkLocal(VarDecl* v);
    void checkLocalDecl(VarDecl* v);
    bool returns(Stmt* s) const;
    void checkFunction(FunctionDecl* f);
    void signature(FunctionDecl* f);

    // Whole program.
    void collect();
    void checkEnums();
    void checkGlobals();
    void checkStructs();
    void extractPlugin();
    void evaluateInfo(FieldDecl* f);
    void evaluatePort(FieldDecl* f);
    void evaluateParam(FieldDecl* f);
    std::string paramMember(Expr* e);
    void resolveActivity();
    std::vector<std::vector<Constant::Condition>> conditions_;  // by parameter index
    void evaluateFile(FieldDecl* f);
    void evaluateSource(FieldDecl* f);
    void evaluateRoute(CallExpr* call, FactoryPatchModel& patch, std::set<std::string>& routed);
    void evaluatePatchFile(CallExpr* call, FactoryPatchModel& patch);
    void evaluatePatches(FunctionDecl* f);
    void buildConstruction();
    void initialValues(const Type* type, int base, Expr* init, StructDecl* scope);
    void contexts();
    void effects();
    void sequencing();
    void checkRules();
    void editorRules();

    // Effects (sema.cpp).
    struct Effects {
        std::vector<Access> reads, writes;
        bool any() const { return !writes.empty(); }
    };
    Effects effectsOf(Expr* e);
    bool storage(Expr* e, Access& out);
    void sequencingIn(Expr* e);
    void sequencingIn(Stmt* s);
    bool conflict(const Effects& a, const Effects& b) const;
    static bool overlaps(const Access& a, const Access& b);
    struct Summary {
        std::set<const FieldDecl*> writesFields;  // members of this the function may change
        bool writesAllOfThis = false;
        std::set<int> writesParams;
    };
    std::map<const FunctionDecl*, Summary> summaries_;
    std::map<const VarDecl*, Access> aliases_;
    void aliasesIn(Stmt* s);
};

}  // namespace supple::scriptc
