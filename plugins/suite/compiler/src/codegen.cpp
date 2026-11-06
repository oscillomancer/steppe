// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Code generation: the analysed tree into MIR, then MIR's generator (or its
// interpreter). Every operation is the one C++ performs, in C++'s order where
// C++ fixes one, and in the types C++ computes in: ints are 32-bit and wrap,
// float arithmetic stays float, no contraction into fused multiply-adds.
// Indexing is checked as script.h checks it. Loops count against the
// instance's budget, so a runaway loop stops the script instead of the audio
// thread.
#include <algorithm>
#include <csetjmp>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <new>
#include <set>

#include "program.h"
#include "sema.h"
#include "supple/script.h"
#include "supple/script_runtime.h"

extern "C" {
#include "mir-gen.h"
#include "mir.h"
}

namespace supple::scriptc {

namespace S = supple::script;
namespace D = supple::script::detail;

namespace {

// MIR reports errors through a function that must not return.
thread_local std::jmp_buf* tMirJump = nullptr;
thread_local char tMirError[512];

extern "C" MIR_NO_RETURN void mirError(MIR_error_type_t, const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vsnprintf(tMirError, sizeof(tMirError), format, args);
    va_end(args);
    std::longjmp(*tMirJump, 1);
}

// MIR's import resolver takes no user data.
thread_local const std::map<std::string, void*>* tImports = nullptr;
void* resolveImport(const char* name) {
    if (!tImports) return nullptr;
    auto it = tImports->find(name);
    return it == tImports->end() ? nullptr : it->second;
}

// Runtime entry points the generated code calls besides the built-ins.
extern "C" void supple_script_oob(int line) { D::outOfRange(line); }
extern "C" void supple_script_budget(void* ctx, int line) {
    auto* in = static_cast<D::Instance*>(ctx);
    if (in->status.budgetExceeded.fetch_add(1, std::memory_order_relaxed) == 0)
        in->status.budgetLine.store(static_cast<std::uint32_t>(line), std::memory_order_relaxed);
}
extern "C" double supple_script_param_at(const S::Param* p, int i, int c) { return p->at(i, c); }
extern "C" void supple_script_transport(const S::Module* p, S::Transport* out) {
    *out = p->transport();
}
extern "C" void supple_script_input(const S::Input* p, int c, S::InputChannel* out) {
    *out = (*p)[c];
}
extern "C" void supple_script_output(const S::Output* p, int c, S::Channel* out) { *out = (*p)[c]; }
extern "C" void supple_script_reset_param(S::Param* p, double* spare) { D::resetMember(*p, spare); }
extern "C" void supple_script_reset_input(S::Input* p, double* spare) { D::resetMember(*p, spare); }
extern "C" void supple_script_reset_output(S::Output* p, double* spare) {
    D::resetMember(*p, spare);
}
extern "C" int supple_script_round(double x) { return S::roundToInt(x); }
extern "C" void supple_script_resize_d(void* b, int n) {
    static_cast<S::Buffer<double>*>(b)->resize(n);
}
extern "C" void supple_script_resize_f(void* b, int n) {
    static_cast<S::Buffer<float>*>(b)->resize(n);
}
extern "C" void supple_script_resize_i(void* b, int n) {
    static_cast<S::Buffer<int>*>(b)->resize(n);
}
extern "C" void supple_script_param_gain(const void* p, double* out) {
    *out = static_cast<const S::Param*>(p)->gain();
}
extern "C" void* supple_script_memcpy(void* d, const void* s, std::size_t n) {
    return std::memcpy(d, s, n);
}

enum class VK { I, F, D };

constexpr int kBudgetOffset = static_cast<int>(offsetof(D::Instance, budget));

bool triviallyCopyable(const Type* t) {
    if (t->isScalar()) return true;
    if (t->kind == TypeKind::Array) return !t->shared && triviallyCopyable(t->element);
    if (t->kind == TypeKind::Object) return t->builtin->triviallyCopyable;
    if (t->kind != TypeKind::Struct) return false;
    for (const FieldDecl* f : t->structDecl->fields)
        if (!f->isStatic && !triviallyCopyable(f->qt.type)) return false;
    return true;
}

}  // namespace

class Codegen {
  public:
    Codegen(Analysis& an, const CompileOptions& options, Diagnostics& diags)
        : an_(an), options_(options), diags_(diags), b_(Builtins::get()) {}

    std::shared_ptr<const Program> run();

  private:
    // --- types ---------------------------------------------------------------------
    static VK kindOf(const Type* t) {
        if (t->kind == TypeKind::Float) return VK::F;
        if (t->kind == TypeKind::Double) return VK::D;
        return VK::I;
    }
    static MIR_type_t regType(VK k) {
        return k == VK::F ? MIR_T_F : k == VK::D ? MIR_T_D : MIR_T_I64;
    }
    static MIR_type_t memType(const Type* t) {
        switch (t->kind) {
            case TypeKind::Bool:
                return MIR_T_U8;
            case TypeKind::Int:
            case TypeKind::Enum:
                return MIR_T_I32;
            case TypeKind::Float:
                return MIR_T_F;
            case TypeKind::Double:
                return MIR_T_D;
            default:
                return MIR_T_P;
        }
    }
    static MIR_type_t abiType(Abi a) {
        switch (a) {
            case Abi::I32:
                return MIR_T_I32;
            case Abi::F32:
                return MIR_T_F;
            case Abi::F64:
                return MIR_T_D;
            case Abi::I64:
                return MIR_T_I64;
            default:
                return MIR_T_P;
        }
    }

    // --- emitting --------------------------------------------------------------------
    MIR_reg_t temp(VK k) {
        char name[24];
        std::snprintf(name, sizeof(name), "t%d", ++temps_);
        return MIR_new_func_reg(ctx_, func_->u.func, regType(k), name);
    }
    MIR_reg_t ptemp() { return temp(VK::I); }
    void emit(MIR_insn_t insn) { MIR_append_insn(ctx_, func_, insn); }
    void emit(MIR_insn_code_t code, MIR_op_t a) { emit(MIR_new_insn(ctx_, code, a)); }
    void emit(MIR_insn_code_t code, MIR_op_t a, MIR_op_t b) {
        emit(MIR_new_insn(ctx_, code, a, b));
    }
    void emit(MIR_insn_code_t code, MIR_op_t a, MIR_op_t b, MIR_op_t c) {
        emit(MIR_new_insn(ctx_, code, a, b, c));
    }
    MIR_op_t reg(MIR_reg_t r) { return MIR_new_reg_op(ctx_, r); }
    MIR_op_t imm(std::int64_t v) { return MIR_new_int_op(ctx_, v); }
    MIR_op_t mem(MIR_type_t t, MIR_reg_t base, std::int64_t disp) {
        return MIR_new_mem_op(ctx_, t, disp, base, 0, 1);
    }
    MIR_op_t label(MIR_label_t l) { return MIR_new_label_op(ctx_, l); }
    MIR_label_t newLabel() { return MIR_new_label(ctx_); }
    void place(MIR_label_t l) { emit(l); }

    MIR_op_t zero(VK k) {
        return k == VK::F   ? MIR_new_float_op(ctx_, 0.0f)
               : k == VK::D ? MIR_new_double_op(ctx_, 0.0)
                            : imm(0);
    }
    MIR_op_t constant(const Expr* e) {
        const Type* t = e->type();
        if (t->kind == TypeKind::Double) return MIR_new_double_op(ctx_, e->constValue);
        if (t->kind == TypeKind::Float)
            return MIR_new_float_op(ctx_, static_cast<float>(e->constValue));
        return imm(t->kind == TypeKind::Bool ? (e->constInt != 0) : e->constInt);
    }
    // A value copied into a fresh register (an operand may be an immediate).
    MIR_reg_t toReg(MIR_op_t op, VK k) {
        if (op.mode == MIR_OP_REG) return op.u.reg;
        MIR_reg_t r = temp(k);
        emit(k == VK::F ? MIR_FMOV : k == VK::D ? MIR_DMOV : MIR_MOV, reg(r), op);
        return r;
    }
    void move(VK k, MIR_op_t dst, MIR_op_t src) {
        emit(k == VK::F ? MIR_FMOV : k == VK::D ? MIR_DMOV : MIR_MOV, dst, src);
    }
    // Wraps a 64-bit result to int's 32 bits, sign-extended (the invariant
    // every int register keeps).
    void wrap32(MIR_reg_t r) { emit(MIR_EXT32, reg(r), reg(r)); }

    // --- imports and prototypes -------------------------------------------------------
    MIR_item_t proto(const std::vector<Abi>& abi) {
        std::string key = "p";
        for (Abi a : abi) key += static_cast<char>('0' + static_cast<int>(a));
        auto it = protos_.find(key);
        if (it != protos_.end()) return it->second;
        std::vector<MIR_var_t> vars;
        names_.push_back(std::make_unique<std::vector<std::string>>());
        auto& owned = *names_.back();
        for (size_t i = 1; i < abi.size(); ++i) owned.push_back("a" + std::to_string(i));
        for (size_t i = 1; i < abi.size(); ++i)
            vars.push_back(MIR_var_t{abiType(abi[i]), owned[i - 1].c_str(), 0});
        MIR_type_t res = abiType(abi[0]);
        MIR_item_t p = MIR_new_proto_arr(ctx_, key.c_str(), abi[0] == Abi::Void ? 0 : 1, &res,
                                         vars.size(), vars.data());
        protos_[key] = p;
        return p;
    }
    MIR_item_t import(void* address) {
        auto it = importItems_.find(address);
        if (it != importItems_.end()) return it->second;
        const std::string name = "imp" + std::to_string(importItems_.size());
        imports_[name] = address;
        MIR_item_t item = MIR_new_import(ctx_, name.c_str());
        importItems_[address] = item;
        return item;
    }
    // Calls a C function: `abi` is its result then its parameters.
    void callC(void* fn, const std::vector<Abi>& abi, std::vector<MIR_op_t> args,
               MIR_reg_t result = 0) {
        std::vector<MIR_op_t> ops{MIR_new_ref_op(ctx_, proto(abi)),
                                  MIR_new_ref_op(ctx_, import(fn))};
        if (abi[0] != Abi::Void) ops.push_back(reg(result));
        ops.insert(ops.end(), args.begin(), args.end());
        emit(MIR_new_insn_arr(ctx_, MIR_CALL, ops.size(), ops.data()));
    }

    // --- places ------------------------------------------------------------------------
    struct Place {
        bool inReg = false;
        MIR_reg_t reg = 0;  // inReg: the value; otherwise the base address
        std::int64_t disp = 0;
    };
    struct Local {
        bool inReg = false;      // a scalar in a register
        MIR_reg_t reg = 0;       // the register, or the reference's address
        bool reference = false;  // reg holds an address
        int frame = -1;          // offset in the frame (memory locals)
    };

    MIR_op_t load(const Type* t, const Place& p) {
        if (p.inReg) return reg(p.reg);
        const VK k = kindOf(t);
        MIR_reg_t r = temp(k);
        move(k, reg(r), mem(memType(t), p.reg, p.disp));
        return reg(r);
    }
    void store(const Type* t, const Place& p, MIR_op_t v) {
        const VK k = kindOf(t);
        if (p.inReg) {
            move(k, reg(p.reg), v);
            return;
        }
        if (t->kind == TypeKind::Bool) {
            // A bool's byte holds 0 or 1.
            emit(MIR_MOV, mem(MIR_T_U8, p.reg, p.disp), v);
            return;
        }
        move(k, mem(memType(t), p.reg, p.disp), v);
    }
    MIR_reg_t addressOf(const Place& p) {
        MIR_reg_t r = ptemp();
        if (p.disp == 0)
            emit(MIR_MOV, reg(r), reg(p.reg));
        else
            emit(MIR_ADD, reg(r), reg(p.reg), imm(p.disp));
        return r;
    }
    void copyBytes(MIR_reg_t dst, MIR_reg_t src, int size) {
        static void* const memcpyFn = reinterpret_cast<void*>(&supple_script_memcpy);
        MIR_reg_t ignored = ptemp();
        callC(memcpyFn, {Abi::Ptr, Abi::Ptr, Abi::Ptr, Abi::Ptr}, {reg(dst), reg(src), imm(size)},
              ignored);
    }

    // --- generation (below) ---
    Place placeOf(Expr* e);
    Place assign(AssignExpr* e);
    Place increment(UnaryExpr* e, MIR_op_t& result);
    MIR_reg_t aggregate(Expr* e);
    MIR_op_t value(Expr* e);
    MIR_op_t convertValue(MIR_op_t v, const Type* from, const Type* to);
    MIR_op_t call(CallExpr* c);
    MIR_op_t intrinsic(CallExpr* c, Intrinsic which);
    MIR_op_t arithmetic(const std::string& op, const Type* t, MIR_op_t a, MIR_op_t b);
    MIR_op_t comparison(const std::string& op, const Type* t, MIR_op_t a, MIR_op_t b);
    Place checkedElement(MIR_reg_t base, MIR_reg_t index, int count, MIR_reg_t countReg,
                         int elementSize, MIR_reg_t spare, const Type* elementType, int line,
                         bool clearSpare, bool members = false);
    void channelOf(Expr* blockOrChannel, MIR_reg_t& data, MIR_reg_t& frames, MIR_reg_t& spare);
    Place channelElement(Expr* indexExpr, bool clearSpare);
    void initialize(MIR_reg_t address, const Type* t, Expr* init, int line);
    void statement(Stmt* s);
    void budgetCheck(int line);
    void abortCheck();
    void function(FunctionDecl* f);
    MIR_reg_t frameSlot(int size, int align, int& offset);
    const unsigned char* prototype(const Type* t);

    Analysis& an_;
    const CompileOptions& options_;
    Diagnostics& diags_;
    const Builtins& b_;
    ProgramData* program_ = nullptr;

    MIR_context_t ctx_ = nullptr;
    MIR_module_t module_ = nullptr;
    std::map<const FunctionDecl*, MIR_item_t> forwards_;
    std::map<const FunctionDecl*, MIR_item_t> items_;
    std::map<const FunctionDecl*, std::string> functionNames_;
    std::map<std::string, MIR_item_t> protos_;
    std::map<const FunctionDecl*, MIR_item_t> functionProtos_;
    std::map<void*, MIR_item_t> importItems_;
    std::map<std::string, void*> imports_;
    std::vector<std::unique_ptr<std::vector<std::string>>> names_;
    std::map<const Type*, const unsigned char*> prototypes_;
    // Each function's frame and the script functions it calls, for the
    // stack a chain of calls needs (stackDepth()).
    std::map<const FunctionDecl*, int> frames_;
    std::map<const FunctionDecl*, std::set<const FunctionDecl*>> callees_;
    int stackDepth(const FunctionDecl* f, std::map<const FunctionDecl*, int>& known) const;

    // Per function.
    FunctionDecl* fn_ = nullptr;
    MIR_item_t func_ = nullptr;
    MIR_reg_t ctxReg_ = 0, selfReg_ = 0, frameReg_ = 0;
    int frameSize_ = 0;
    int temps_ = 0;
    std::map<const VarDecl*, Local> locals_;
    MIR_label_t exit_ = nullptr;
    std::vector<std::pair<MIR_label_t, MIR_label_t>> loops_;  // break, continue
    std::vector<MIR_label_t> breaks_;
};

// --- the frame ----------------------------------------------------------------------------

MIR_reg_t Codegen::frameSlot(int size, int align, int& offset) {
    align = std::max(8, align);
    frameSize_ = (frameSize_ + align - 1) / align * align;
    if (std::max(size, 1) > kMaxFrameBytes - frameSize_) {
        diags_.error(fn_->loc, "script.local.size",
                     fn_->name + "() needs more than 64 KiB of local storage and temporaries",
                     "move large arrays or structs to plugin members, or reuse scratch storage");
        // The program will be discarded before linking. Keep generating
        // within the limit so invalid input cannot overflow frameSize_.
        offset = 0;
        return frameReg_;
    }
    offset = frameSize_;
    frameSize_ += std::max(size, 1);
    MIR_reg_t r = ptemp();
    emit(MIR_ADD, reg(r), reg(frameReg_), imm(offset));
    return r;
}

// An image of a value's starting state, for locals: zeros, the starting
// values of its members, its runtime objects constructed.
const unsigned char* Codegen::prototype(const Type* t) {
    auto it = prototypes_.find(t);
    if (it != prototypes_.end()) return it->second;
    auto image = std::make_unique<unsigned char[]>(static_cast<size_t>(std::max(1, t->size)));
    std::memset(image.get(), 0, static_cast<size_t>(std::max(1, t->size)));
    std::function<void(const Type*, int, Expr*)> fill = [&](const Type* type, int base,
                                                            Expr* init) {
        if (type->isScalar()) {
            if (!init || !init->constant) return;
            unsigned char* at = image.get() + base;
            switch (type->kind) {
                case TypeKind::Bool:
                    *at = init->constInt != 0;
                    break;
                case TypeKind::Int:
                case TypeKind::Enum: {
                    const auto v = static_cast<std::int32_t>(init->constInt);
                    std::memcpy(at, &v, 4);
                    break;
                }
                case TypeKind::Float: {
                    const auto v = static_cast<float>(init->constValue);
                    std::memcpy(at, &v, 4);
                    break;
                }
                case TypeKind::Double:
                    std::memcpy(at, &init->constValue, 8);
                    break;
                default:
                    break;
            }
            return;
        }
        if (type->kind == TypeKind::Array) {
            auto* list = init && init->kind == ExprKind::InitList ? static_cast<InitListExpr*>(init)
                                                                  : nullptr;
            for (int i = 0; i <= type->count; ++i)
                fill(type->element, base + i * type->element->size,
                     list && i < static_cast<int>(list->items.size())
                         ? list->items[static_cast<size_t>(i)].value
                         : nullptr);
            return;
        }
        if (type->kind == TypeKind::Struct) {
            std::map<std::string, Expr*> given;
            if (init && init->kind == ExprKind::InitList)
                for (auto& item : static_cast<InitListExpr*>(init)->items)
                    given[item.designator] = item.value;
            for (FieldDecl* f : type->structDecl->fields) {
                if (f->isStatic || !f->qt.type) continue;
                auto g = given.find(f->name);
                fill(f->qt.type, base + f->offset, g != given.end() ? g->second : f->init);
            }
            return;
        }
        if (type->kind == TypeKind::Object && type->builtin->construct)
            type->builtin->construct(image.get() + base);
    };
    fill(t, 0, nullptr);
    const unsigned char* raw = image.get();
    program_->prototypes.push_back(std::move(image));
    prototypes_[t] = raw;
    return raw;
}

// --- places ---------------------------------------------------------------------------------

Codegen::Place Codegen::placeOf(Expr* e) {
    switch (e->kind) {
        case ExprKind::Name: {
            auto* n = static_cast<NameExpr*>(e);
            if (n->ref == RefKind::Local) {
                const Local& l = locals_[n->var];
                if (l.inReg) return Place{true, l.reg, 0};
                if (l.reference) return Place{false, l.reg, 0};
                return Place{false, frameReg_, l.frame};
            }
            if (n->ref == RefKind::Field) return Place{false, selfReg_, n->field->offset};
            break;
        }
        case ExprKind::Member: {
            auto* m = static_cast<MemberExpr*>(e);
            if (m->ref == RefKind::Field) {
                Place p = placeOf(m->object);
                p.disp += m->field->offset;
                return p;
            }
            if (m->ref == RefKind::BuiltinField) {
                const Type* ot = m->object->type();
                Place p = placeOf(m->object);
                if (p.inReg) {
                    // A runtime record held in a register: never (they live in memory).
                    break;
                }
                p.disp += ot->builtin->fields[static_cast<size_t>(m->builtinField)].offset;
                return p;
            }
            break;
        }
        case ExprKind::Index: {
            auto* x = static_cast<IndexExpr*>(e);
            const Type* ot = x->object->type();
            if (ot->kind == TypeKind::Object && ot->builtin == b_.channel())
                return channelElement(x, true);
            if (ot->kind == TypeKind::Array) {
                Place base = placeOf(x->object);
                MIR_reg_t addr = addressOf(base);
                MIR_reg_t index = toReg(value(x->index), VK::I);
                MIR_reg_t spare = ptemp();
                emit(MIR_ADD, reg(spare), reg(addr),
                     imm(static_cast<std::int64_t>(ot->count) * ot->element->size));
                return checkedElement(addr, index, ot->count, 0, ot->element->size, spare,
                                      ot->element, x->loc.line, true, ot->members);
            }
            if (ot->kind == TypeKind::Buffer) {
                Place base = placeOf(x->object);
                MIR_reg_t object = addressOf(base);
                MIR_reg_t items = ptemp(), size = ptemp(), spare = ptemp();
                const Type* el = ot->element;
                int itemsOff = 0, sizeOff = 0, spareOff = 0;
                if (el->kind == TypeKind::Double) {
                    itemsOff = D::Layout::bufferItems<double>();
                    sizeOff = D::Layout::bufferSize<double>();
                    spareOff = D::Layout::bufferSpare<double>();
                } else if (el->kind == TypeKind::Float) {
                    itemsOff = D::Layout::bufferItems<float>();
                    sizeOff = D::Layout::bufferSize<float>();
                    spareOff = D::Layout::bufferSpare<float>();
                } else {
                    itemsOff = D::Layout::bufferItems<int>();
                    sizeOff = D::Layout::bufferSize<int>();
                    spareOff = D::Layout::bufferSpare<int>();
                }
                emit(MIR_MOV, reg(items), mem(MIR_T_P, object, itemsOff));
                emit(MIR_MOV, reg(size), mem(MIR_T_I32, object, sizeOff));
                emit(MIR_ADD, reg(spare), reg(object), imm(spareOff));
                MIR_reg_t index = toReg(value(x->index), VK::I);
                return checkedElement(items, index, -1, size, el->size, spare, el, x->loc.line,
                                      true);
            }
            break;
        }
        case ExprKind::Assign: {
            auto* a = static_cast<AssignExpr*>(e);
            return assign(a);
        }
        case ExprKind::Unary: {
            auto* u = static_cast<UnaryExpr*>(e);
            if (!u->postfix) {
                MIR_op_t ignored{};
                return increment(u, ignored);
            }
            break;
        }
        case ExprKind::Conditional: {
            auto* c = static_cast<ConditionalExpr*>(e);
            MIR_reg_t addr = ptemp();
            MIR_label_t otherwise = newLabel(), done = newLabel();
            emit(MIR_BF, label(otherwise), value(c->cond));
            emit(MIR_MOV, reg(addr), reg(addressOf(placeOf(c->then))));
            emit(MIR_JMP, label(done));
            place(otherwise);
            emit(MIR_MOV, reg(addr), reg(addressOf(placeOf(c->otherwise))));
            place(done);
            return Place{false, addr, 0};
        }
        case ExprKind::Cast:
            return placeOf(static_cast<CastExpr*>(e)->operand);
        default:
            break;
    }
    // A value that is not an lvalue but lives in memory: a temporary.
    return Place{false, aggregate(e), 0};
}

// The address of a value of class type, materialised when it is a temporary.
MIR_reg_t Codegen::aggregate(Expr* e) {
    const Type* t = e->type();
    if (e->lvalue && e->kind != ExprKind::Call) return addressOf(placeOf(e));
    int offset = 0;
    MIR_reg_t addr = frameSlot(t->size, t->align, offset);
    if (e->kind == ExprKind::Call) {
        auto* call = static_cast<CallExpr*>(e);
        if (call->builtinMethod && call->builtinMethod->intrinsic == Intrinsic::ModuleTransport) {
            MIR_reg_t self = call->object ? aggregate(call->object) : selfReg_;
            callC(reinterpret_cast<void*>(&supple_script_transport),
                  {Abi::Void, Abi::Ptr, Abi::Ptr}, {reg(self), reg(addr)});
            return addr;
        }
    }
    if (t->kind == TypeKind::Object &&
        (t->builtin == b_.channel() || t->builtin == b_.inputChannel())) {
        MIR_reg_t data = 0, frames = 0, spare = 0;
        channelOf(e, data, frames, spare);
        if (t->builtin == b_.channel()) {
            emit(MIR_MOV, mem(MIR_T_P, addr, offsetof(S::Channel, data_)), reg(data));
            emit(MIR_MOV, mem(MIR_T_I32, addr, offsetof(S::Channel, frames_)), reg(frames));
            emit(MIR_MOV, mem(MIR_T_P, addr, offsetof(S::Channel, spare_)), reg(spare));
        } else {
            emit(MIR_MOV, mem(MIR_T_P, addr, offsetof(S::InputChannel, data_)), reg(data));
            emit(MIR_MOV, mem(MIR_T_I32, addr, offsetof(S::InputChannel, frames_)), reg(frames));
        }
        return addr;
    }
    initialize(addr, t, e, e->loc.line);
    return addr;
}

// Writes a value of type t, described by `init`, at `address`: a copy of
// another value, braces, or the type's starting state.
void Codegen::initialize(MIR_reg_t address, const Type* t, Expr* init, int line) {
    // Text (const char*) is a pointer value and is stored as a scalar is: a
    // literal or format() has no memory of its own to copy from, and
    // aggregate() would hand it straight back here.
    if (t->isScalar() || t->kind == TypeKind::String) {
        store(t, Place{false, address, 0}, init ? value(init) : zero(kindOf(t)));
        return;
    }
    // Start from the type's starting state, then apply what is given.
    auto* list =
        init && init->kind == ExprKind::InitList ? static_cast<InitListExpr*>(init) : nullptr;
    if (init && init->kind == ExprKind::Cast && static_cast<CastExpr*>(init)->operand &&
        static_cast<CastExpr*>(init)->operand->kind == ExprKind::InitList)
        list = static_cast<InitListExpr*>(static_cast<CastExpr*>(init)->operand);
    if (!init || list) {
        MIR_reg_t proto = ptemp();
        emit(MIR_MOV, reg(proto), imm(reinterpret_cast<std::int64_t>(prototype(t))));
        copyBytes(address, proto, t->size);
        if (!list) return;
        if (t->kind == TypeKind::Array) {
            for (size_t i = 0; i < list->items.size(); ++i) {
                MIR_reg_t at = ptemp();
                emit(MIR_ADD, reg(at), reg(address),
                     imm(static_cast<std::int64_t>(i) * t->element->size));
                initialize(at, t->element, list->items[i].value, line);
            }
        } else if (t->kind == TypeKind::Struct) {
            for (auto& item : list->items) {
                FieldDecl* f = nullptr;
                for (FieldDecl* x : t->structDecl->fields)
                    if (x->name == item.designator) f = x;
                if (!f) continue;
                MIR_reg_t at = ptemp();
                emit(MIR_ADD, reg(at), reg(address), imm(f->offset));
                initialize(at, f->qt.type, item.value, line);
            }
        }
        return;
    }
    // A copy of another value of the same type (copyable: plain bytes and
    // trivially copyable runtime objects).
    MIR_reg_t src = aggregate(init);
    copyBytes(address, src, t->size);
}

// Bounds-checked element address: count >= 0 is a constant size, otherwise
// countReg holds it. Out of range: the spare element (reset to its defaults
// first when trivially copyable), and the line reported.
Codegen::Place Codegen::checkedElement(MIR_reg_t base, MIR_reg_t index, int count,
                                       MIR_reg_t countReg, int elementSize, MIR_reg_t spare,
                                       const Type* elementType, int line, bool clearSpare,
                                       bool members) {
    MIR_reg_t addr = ptemp();
    MIR_label_t bad = newLabel(), done = newLabel();
    if (count >= 0)
        emit(MIR_UBGE, label(bad), reg(index), imm(count));
    else
        emit(MIR_UBGE, label(bad), reg(index), reg(countReg));
    MIR_reg_t offset = ptemp();
    emit(MIR_MUL, reg(offset), reg(index), imm(elementSize));
    emit(MIR_ADD, reg(addr), reg(base), reg(offset));
    emit(MIR_JMP, label(done));
    place(bad);
    callC(reinterpret_cast<void*>(&supple_script_oob), {Abi::Void, Abi::I32}, {imm(line)});
    emit(MIR_MOV, reg(addr), reg(spare));
    if (members) {
        MIR_reg_t sample = ptemp();
        emit(MIR_ADD, reg(sample), reg(spare), imm(elementType->size));
        void* reset = elementType->builtin == b_.param()
                          ? reinterpret_cast<void*>(&supple_script_reset_param)
                      : elementType->builtin == b_.cls("Input")
                          ? reinterpret_cast<void*>(&supple_script_reset_input)
                          : reinterpret_cast<void*>(&supple_script_reset_output);
        callC(reset, {Abi::Void, Abi::Ptr, Abi::Ptr}, {reg(spare), reg(sample)});
    } else if (clearSpare && triviallyCopyable(elementType))
        initialize(addr, elementType, nullptr, line);
    place(done);
    return Place{false, addr, 0};
}

// A Channel or InputChannel's data, frames and spare, from io[c],
// io.sidechain(c), io.output(bus, c) or a Channel variable.
void Codegen::channelOf(Expr* e, MIR_reg_t& data, MIR_reg_t& frames, MIR_reg_t& spare) {
    data = ptemp();
    frames = ptemp();
    spare = ptemp();
    auto fromBlock = [&](MIR_reg_t block, MIR_reg_t index, std::int64_t pointersOffset,
                         std::int64_t countOffset, bool countIsChannels, bool input) {
        MIR_label_t bad = newLabel(), done = newLabel();
        MIR_reg_t count = ptemp();
        if (countIsChannels)
            emit(MIR_MOV, reg(count), mem(MIR_T_I32, block, offsetof(S::Block, channels)));
        else
            emit(MIR_MOV, reg(count), mem(MIR_T_I32, block, countOffset));
        emit(MIR_MOV, reg(spare), mem(MIR_T_P, block, offsetof(S::Block, spare_)));
        emit(MIR_UBGE, label(bad), reg(index), reg(count));
        MIR_reg_t pointers = ptemp();
        emit(MIR_MOV, reg(pointers), mem(MIR_T_P, block, pointersOffset));
        emit(MIR_MOV, reg(data), MIR_new_mem_op(ctx_, MIR_T_P, 0, pointers, index, 8));
        emit(MIR_MOV, reg(frames), mem(MIR_T_I32, block, offsetof(S::Block, frames)));
        emit(MIR_JMP, label(done));
        place(bad);
        emit(MIR_MOV, reg(data), input ? imm(0) : reg(spare));
        emit(MIR_MOV, reg(frames), imm(0));
        place(done);
    };
    if (e->kind == ExprKind::Index) {
        auto* x = static_cast<IndexExpr*>(e);
        MIR_reg_t block = aggregate(x->object);
        MIR_reg_t index = toReg(value(x->index), VK::I);
        if (b_.isPort(x->object->type())) {
            const bool input = x->object->type()->builtin == b_.cls("Input");
            int offset = 0;
            MIR_reg_t view =
                frameSlot(input ? sizeof(S::InputChannel) : sizeof(S::Channel), 8, offset);
            callC(input ? reinterpret_cast<void*>(&supple_script_input)
                        : reinterpret_cast<void*>(&supple_script_output),
                  {Abi::Void, Abi::Ptr, Abi::I32, Abi::Ptr}, {reg(block), reg(index), reg(view)});
            // Both views begin with data_, frames_; Channel also has spare_.
            emit(MIR_MOV, reg(data), mem(MIR_T_P, view, offsetof(S::InputChannel, data_)));
            emit(MIR_MOV, reg(frames), mem(MIR_T_I32, view, offsetof(S::InputChannel, frames_)));
            if (input)
                emit(MIR_MOV, reg(spare), imm(0));
            else
                emit(MIR_MOV, reg(spare), mem(MIR_T_P, view, offsetof(S::Channel, spare_)));
            return;
        }
        fromBlock(block, index, offsetof(S::Block, data_), 0, true, false);
        return;
    }
    if (e->kind == ExprKind::Call) {
        auto* c = static_cast<CallExpr*>(e);
        MIR_reg_t block = aggregate(c->object);
        if (c->builtinMethod && c->builtinMethod->intrinsic == Intrinsic::BlockSidechain) {
            MIR_reg_t index = toReg(value(c->args[0]), VK::I);
            fromBlock(block, index, offsetof(S::Block, sidechain_),
                      offsetof(S::Block, sidechainChannels_), false, true);
            return;
        }
        // io.output(bus, c): aux_[bus][c], both checked.
        MIR_reg_t bus = toReg(value(c->args[0]), VK::I);
        MIR_reg_t channel = toReg(value(c->args[1]), VK::I);
        MIR_label_t bad = newLabel(), done = newLabel();
        MIR_reg_t outputs = ptemp(), channels = ptemp(), aux = ptemp(), busPointers = ptemp();
        emit(MIR_MOV, reg(spare), mem(MIR_T_P, block, offsetof(S::Block, spare_)));
        emit(MIR_MOV, reg(outputs), mem(MIR_T_I32, block, offsetof(S::Block, outputs_)));
        emit(MIR_MOV, reg(channels), mem(MIR_T_I32, block, offsetof(S::Block, channels)));
        emit(MIR_UBGE, label(bad), reg(bus), reg(outputs));
        emit(MIR_UBGE, label(bad), reg(channel), reg(channels));
        emit(MIR_MOV, reg(aux), mem(MIR_T_P, block, offsetof(S::Block, aux_)));
        emit(MIR_MOV, reg(busPointers), MIR_new_mem_op(ctx_, MIR_T_P, 0, aux, bus, 8));
        emit(MIR_MOV, reg(data), MIR_new_mem_op(ctx_, MIR_T_P, 0, busPointers, channel, 8));
        emit(MIR_MOV, reg(frames), mem(MIR_T_I32, block, offsetof(S::Block, frames)));
        emit(MIR_JMP, label(done));
        place(bad);
        emit(MIR_MOV, reg(data), reg(spare));
        emit(MIR_MOV, reg(frames), imm(0));
        place(done);
        return;
    }
    // A Channel or InputChannel held in a variable.
    MIR_reg_t at = addressOf(placeOf(e));
    if (e->type()->builtin == b_.channel()) {
        emit(MIR_MOV, reg(data), mem(MIR_T_P, at, offsetof(S::Channel, data_)));
        emit(MIR_MOV, reg(frames), mem(MIR_T_I32, at, offsetof(S::Channel, frames_)));
        emit(MIR_MOV, reg(spare), mem(MIR_T_P, at, offsetof(S::Channel, spare_)));
    } else {
        emit(MIR_MOV, reg(data), mem(MIR_T_P, at, offsetof(S::InputChannel, data_)));
        emit(MIR_MOV, reg(frames), mem(MIR_T_I32, at, offsetof(S::InputChannel, frames_)));
        emit(MIR_MOV, reg(spare), imm(0));
    }
}

// io[c][i], or ch[i] for a Channel variable: a checked sample.
Codegen::Place Codegen::channelElement(Expr* e, bool clearSpare) {
    auto* x = static_cast<IndexExpr*>(e);
    MIR_reg_t data = 0, frames = 0, spare = 0;
    channelOf(x->object, data, frames, spare);
    MIR_reg_t index = toReg(value(x->index), VK::I);
    if (x->object->type()->builtin == b_.inputChannel()) {
        // InputChannel has no writable spare. Materialized views set that
        // pointer to null, and direct views may share a Block's dirty spare.
        // Both must read zero when no input sample exists.
        int offset = 0;
        spare = frameSlot(8, 8, offset);
        clearSpare = true;
    }
    return checkedElement(data, index, -1, frames, 8, spare, b_.doubleType(), x->loc.line,
                          clearSpare);
}

// An assignment and a prefix increment are themselves lvalues. Keep the
// place computed during their side effect; evaluating the target again
// would repeat subscripts, calls and increments.
Codegen::Place Codegen::assign(AssignExpr* a) {
    const Type* tt = a->target->type();
    if (a->op == "=") {
        if (tt->isScalar() || tt->kind == TypeKind::String) {
            // C++17: the value is computed before the target.
            MIR_op_t v = value(a->value);
            if (v.mode == MIR_OP_REG) {
                MIR_reg_t saved = temp(kindOf(tt));
                move(kindOf(tt), reg(saved), v);
                v = reg(saved);
            }
            Place p = placeOf(a->target);
            store(tt, p, v);
            return p;
        }
        MIR_reg_t src = 0;
        if (a->value->kind == ExprKind::InitList) {
            int off = 0;
            src = frameSlot(tt->size, tt->align, off);
            initialize(src, tt, a->value, a->loc.line);
        } else {
            src = aggregate(a->value);
        }
        Place p = placeOf(a->target);
        MIR_reg_t dst = addressOf(p);
        copyBytes(dst, src, tt->size);
        return p;
    }
    const std::string op = a->op.substr(0, a->op.size() - 1);
    MIR_op_t v = value(a->value);
    if (v.mode == MIR_OP_REG) {
        MIR_reg_t saved = temp(kindOf(a->value->type()));
        move(kindOf(a->value->type()), reg(saved), v);
        v = reg(saved);
    }
    Place p = placeOf(a->target);
    MIR_op_t cur = convertValue(load(tt, p), tt, (op == "<<" || op == ">>") ? tt : a->operandType);
    MIR_op_t result = arithmetic(op, (op == "<<" || op == ">>") ? tt : a->operandType, cur, v);
    MIR_op_t back = convertValue(result, (op == "<<" || op == ">>") ? tt : a->operandType, tt);
    store(tt, p, back);
    return p;
}

Codegen::Place Codegen::increment(UnaryExpr* u, MIR_op_t& result) {
    const Type* ot = u->operand->type();
    Place p = placeOf(u->operand);
    MIR_op_t old = load(ot, p);
    const VK k = kindOf(ot);
    MIR_reg_t oldCopy = toReg(old, k);
    if (p.inReg) {
        // The register itself changes; keep the old value apart.
        MIR_reg_t saved = temp(k);
        move(k, reg(saved), reg(oldCopy));
        oldCopy = saved;
    }
    const MIR_op_t one = k == VK::D   ? MIR_new_double_op(ctx_, 1.0)
                         : k == VK::F ? MIR_new_float_op(ctx_, 1.0f)
                                      : imm(1);
    MIR_op_t next = arithmetic(u->op == "++" ? "+" : "-", ot, reg(oldCopy), one);
    store(ot, p, next);
    result = u->postfix ? reg(oldCopy) : next;
    return p;
}

// --- values ----------------------------------------------------------------------------------

MIR_op_t Codegen::convertValue(MIR_op_t v, const Type* from, const Type* to) {
    if (from == to || (from->isIntegral() && to->kind == TypeKind::Int) ||
        (from->kind == TypeKind::Enum && to->kind == TypeKind::Int) ||
        (from->kind == TypeKind::Int && to->kind == TypeKind::Enum))
        return v;
    const VK fk = kindOf(from), tk = kindOf(to);
    MIR_reg_t r = temp(tk);
    if (to->kind == TypeKind::Bool) {
        // Nonzero is true; NaN is nonzero.
        if (fk == VK::D)
            emit(MIR_DNE, reg(r), v, MIR_new_double_op(ctx_, 0.0));
        else if (fk == VK::F)
            emit(MIR_FNE, reg(r), v, MIR_new_float_op(ctx_, 0.0f));
        else
            emit(MIR_NE, reg(r), v, imm(0));
        return reg(r);
    }
    if (fk == VK::I && tk == VK::D)
        emit(MIR_I2D, reg(r), v);
    else if (fk == VK::I && tk == VK::F)
        emit(MIR_I2F, reg(r), v);
    else if (fk == VK::F && tk == VK::D)
        emit(MIR_F2D, reg(r), v);
    else if (fk == VK::D && tk == VK::F)
        emit(MIR_D2F, reg(r), v);
    else
        move(tk, reg(r), v);  // never: float to int is refused by sema
    return reg(r);
}

MIR_op_t Codegen::arithmetic(const std::string& op, const Type* t, MIR_op_t a, MIR_op_t b) {
    const VK k = kindOf(t);
    MIR_reg_t r = temp(k);
    if (k == VK::D) {
        emit(op == "+"   ? MIR_DADD
             : op == "-" ? MIR_DSUB
             : op == "*" ? MIR_DMUL
                         : MIR_DDIV,
             reg(r), a, b);
        return reg(r);
    }
    if (k == VK::F) {
        emit(op == "+"   ? MIR_FADD
             : op == "-" ? MIR_FSUB
             : op == "*" ? MIR_FMUL
                         : MIR_FDIV,
             reg(r), a, b);
        return reg(r);
    }
    if (op == "+")
        emit(MIR_ADD, reg(r), a, b), wrap32(r);
    else if (op == "-")
        emit(MIR_SUB, reg(r), a, b), wrap32(r);
    else if (op == "*")
        emit(MIR_MUL, reg(r), a, b), wrap32(r);
    else if (op == "/")
        emit(MIR_DIVS, reg(r), a, b), wrap32(r);
    else if (op == "%")
        emit(MIR_MODS, reg(r), a, b), wrap32(r);
    else if (op == "&")
        emit(MIR_AND, reg(r), a, b);
    else if (op == "|")
        emit(MIR_OR, reg(r), a, b);
    else if (op == "^")
        emit(MIR_XOR, reg(r), a, b);
    else if (op == "<<")
        emit(MIR_LSH, reg(r), a, b), wrap32(r);
    else if (op == ">>")
        emit(MIR_RSH, reg(r), a, b);
    return reg(r);
}

MIR_op_t Codegen::comparison(const std::string& op, const Type* t, MIR_op_t a, MIR_op_t b) {
    const VK k = kindOf(t);
    MIR_reg_t r = temp(VK::I);
    MIR_insn_code_t code;
    if (k == VK::D)
        code = op == "<"    ? MIR_DLT
               : op == "<=" ? MIR_DLE
               : op == ">"  ? MIR_DGT
               : op == ">=" ? MIR_DGE
               : op == "==" ? MIR_DEQ
                            : MIR_DNE;
    else if (k == VK::F)
        code = op == "<"    ? MIR_FLT
               : op == "<=" ? MIR_FLE
               : op == ">"  ? MIR_FGT
               : op == ">=" ? MIR_FGE
               : op == "==" ? MIR_FEQ
                            : MIR_FNE;
    else
        code = op == "<"    ? MIR_LT
               : op == "<=" ? MIR_LE
               : op == ">"  ? MIR_GT
               : op == ">=" ? MIR_GE
               : op == "==" ? MIR_EQ
                            : MIR_NE;
    emit(code, reg(r), a, b);
    return reg(r);
}

MIR_op_t Codegen::value(Expr* e) {
    const Type* t = e->type();
    if (e->constant && t && t->isScalar()) return constant(e);
    switch (e->kind) {
        case ExprKind::StringLit: {
            // Text the program keeps, for as long as its code exists.
            program_->strings.push_back(
                std::make_unique<std::string>(static_cast<StringLitExpr*>(e)->value));
            return imm(reinterpret_cast<std::int64_t>(program_->strings.back()->c_str()));
        }
        case ExprKind::Name: {
            auto* n = static_cast<NameExpr*>(e);
            if (n->ref == RefKind::Local || n->ref == RefKind::Field) return load(t, placeOf(e));
            break;
        }
        case ExprKind::Member: {
            auto* m = static_cast<MemberExpr*>(e);
            if (m->ref == RefKind::Field || m->ref == RefKind::BuiltinField)
                return load(t, placeOf(e));
            break;
        }
        case ExprKind::Index: {
            auto* x = static_cast<IndexExpr*>(e);
            const Type* ot = x->object->type();
            if (ot->kind == TypeKind::Object &&
                (ot->builtin == b_.channel() || ot->builtin == b_.inputChannel()))
                return load(t, channelElement(x, ot->builtin == b_.channel()));
            return load(t, placeOf(e));
        }
        case ExprKind::Call:
            return call(static_cast<CallExpr*>(e));
        case ExprKind::Cast: {
            auto* c = static_cast<CastExpr*>(e);
            if (c->operand && c->operand->kind == ExprKind::InitList) {
                if (t->isScalar()) {
                    auto* list = static_cast<InitListExpr*>(c->operand);
                    return list->items.empty() ? zero(kindOf(t)) : value(list->items[0].value);
                }
                break;
            }
            if (c->userConversion) {
                // Param's operator double: its value.
                Place p = placeOf(c->operand);
                MIR_reg_t r = temp(VK::D);
                emit(MIR_DMOV, reg(r), mem(MIR_T_D, p.reg, p.disp + D::Layout::paramValue));
                return reg(r);
            }
            return convertValue(value(c->operand), c->operand->type(), t);
        }
        case ExprKind::Unary: {
            auto* u = static_cast<UnaryExpr*>(e);
            if (u->op == "++" || u->op == "--") {
                MIR_op_t result{};
                increment(u, result);
                return result;
            }
            MIR_op_t v = value(u->operand);
            const VK k = kindOf(t);
            MIR_reg_t r = temp(k);
            if (u->op == "-") {
                if (k == VK::D)
                    emit(MIR_DNEG, reg(r), v);
                else if (k == VK::F)
                    emit(MIR_FNEG, reg(r), v);
                else
                    emit(MIR_NEG, reg(r), v), wrap32(r);
            } else if (u->op == "+") {
                move(k, reg(r), v);
            } else if (u->op == "!") {
                emit(MIR_EQ, reg(r), v, imm(0));
            } else if (u->op == "~") {
                emit(MIR_XOR, reg(r), v, imm(-1));
            }
            return reg(r);
        }
        case ExprKind::Binary: {
            auto* bin = static_cast<BinaryExpr*>(e);
            const std::string& op = bin->op;
            if (op == "&&" || op == "||") {
                MIR_reg_t r = temp(VK::I);
                MIR_label_t done = newLabel();
                emit(MIR_MOV, reg(r), value(bin->left));
                emit(op == "&&" ? MIR_BF : MIR_BT, label(done), reg(r));
                emit(MIR_MOV, reg(r), value(bin->right));
                place(done);
                return reg(r);
            }
            if (op == ",") {
                if (bin->left->type()->isScalar() || bin->left->kind == ExprKind::Call)
                    value(bin->left);
                return value(bin->right);
            }
            // Sema has refused operands whose effects could interfere, so
            // evaluating left then right is what any C++ order would give.
            MIR_op_t a = value(bin->left);
            MIR_op_t c = value(bin->right);
            const bool cmp =
                op == "<" || op == "<=" || op == ">" || op == ">=" || op == "==" || op == "!=";
            if (cmp) return comparison(op, bin->operandType, a, c);
            return arithmetic(op, bin->operandType, a, c);
        }
        case ExprKind::Assign: {
            Place p = assign(static_cast<AssignExpr*>(e));
            return t->isScalar() || t->kind == TypeKind::String ? load(t, p) : imm(0);
        }
        case ExprKind::Conditional: {
            auto* c = static_cast<ConditionalExpr*>(e);
            const VK k = kindOf(t);
            MIR_reg_t r = temp(k);
            MIR_label_t otherwise = newLabel(), done = newLabel();
            emit(MIR_BF, label(otherwise), value(c->cond));
            move(k, reg(r), value(c->then));
            emit(MIR_JMP, label(done));
            place(otherwise);
            move(k, reg(r), value(c->otherwise));
            place(done);
            return reg(r);
        }
        default:
            break;
    }
    if (!t || !t->isScalar()) {
        // Evaluated for its effects only.
        if (e->kind == ExprKind::Call) call(static_cast<CallExpr*>(e));
        return imm(0);
    }
    diags_.error(e->loc, "script.internal", "internal error: no code for this expression",
                 "please report it");
    return zero(kindOf(t));
}

// --- calls ------------------------------------------------------------------------------------

MIR_op_t Codegen::intrinsic(CallExpr* c, Intrinsic which) {
    const Type* t = c->type();
    auto args = [&](size_t i) { return value(c->args[i]); };
    switch (which) {
        case Intrinsic::Abs: {
            const VK k = kindOf(t);
            MIR_reg_t x = toReg(args(0), k);
            MIR_reg_t r = temp(k);
            MIR_label_t done = newLabel();
            move(k, reg(r), reg(x));
            MIR_op_t z = zero(k);
            MIR_reg_t lt = temp(VK::I);
            emit(k == VK::D ? MIR_DLT : k == VK::F ? MIR_FLT : MIR_LT, reg(lt), reg(x), z);
            emit(MIR_BF, label(done), reg(lt));
            if (k == VK::D)
                emit(MIR_DNEG, reg(r), reg(x));
            else
                emit(MIR_NEG, reg(r), reg(x)), wrap32(r);
            place(done);
            return reg(r);
        }
        case Intrinsic::Min:
        case Intrinsic::Max: {
            const VK k = kindOf(t);
            MIR_reg_t a = toReg(args(0), k);
            if (c->args[0]->lvalue) {
                MIR_reg_t copy = temp(k);
                move(k, reg(copy), reg(a));
                a = copy;
            }
            MIR_reg_t bb = toReg(args(1), k);
            MIR_reg_t r = temp(k);
            MIR_label_t done = newLabel();
            // min: b < a ? b : a;  max: a < b ? b : a.
            MIR_reg_t cond = temp(VK::I);
            if (which == Intrinsic::Min)
                emit(k == VK::D ? MIR_DLT : MIR_LT, reg(cond), reg(bb), reg(a));
            else
                emit(k == VK::D ? MIR_DLT : MIR_LT, reg(cond), reg(a), reg(bb));
            move(k, reg(r), reg(bb));
            emit(MIR_BT, label(done), reg(cond));
            move(k, reg(r), reg(a));
            place(done);
            return reg(r);
        }
        case Intrinsic::Clamp: {
            const VK k = kindOf(t);
            const auto lt = k == VK::D ? MIR_DLT : MIR_LT;
            MIR_reg_t x = toReg(args(0), k);
            if (c->args[0]->lvalue) {
                MIR_reg_t copy = temp(k);
                move(k, reg(copy), reg(x));
                x = copy;
            }
            MIR_reg_t lo = toReg(args(1), k);
            if (c->args[1]->lvalue) {
                MIR_reg_t copy = temp(k);
                move(k, reg(copy), reg(lo));
                lo = copy;
            }
            MIR_reg_t hi = toReg(args(2), k);
            MIR_reg_t r = temp(k), cond = temp(VK::I);
            MIR_label_t done = newLabel(), notLow = newLabel();
            // x < lo ? lo : (hi < x ? hi : x)
            emit(lt, reg(cond), reg(x), reg(lo));
            move(k, reg(r), reg(lo));
            emit(MIR_BT, label(done), reg(cond));
            place(notLow);
            emit(lt, reg(cond), reg(hi), reg(x));
            move(k, reg(r), reg(hi));
            emit(MIR_BT, label(done), reg(cond));
            move(k, reg(r), reg(x));
            place(done);
            return reg(r);
        }
        case Intrinsic::Lerp: {
            // a + (b - a) * t
            MIR_reg_t a = toReg(args(0), VK::D);
            if (c->args[0]->lvalue) {
                MIR_reg_t copy = temp(VK::D);
                move(VK::D, reg(copy), reg(a));
                a = copy;
            }
            MIR_reg_t bb = toReg(args(1), VK::D);
            if (c->args[1]->lvalue) {
                MIR_reg_t copy = temp(VK::D);
                move(VK::D, reg(copy), reg(bb));
                bb = copy;
            }
            MIR_op_t tt = args(2);
            MIR_reg_t d = temp(VK::D), m = temp(VK::D), r = temp(VK::D);
            emit(MIR_DSUB, reg(d), reg(bb), reg(a));
            emit(MIR_DMUL, reg(m), reg(d), tt);
            emit(MIR_DADD, reg(r), reg(a), reg(m));
            return reg(r);
        }
        case Intrinsic::Sign: {
            // x > 0 ? 1 : (x < 0 ? -1 : 0)
            MIR_reg_t x = toReg(args(0), VK::D);
            MIR_reg_t r = temp(VK::D), cond = temp(VK::I);
            MIR_label_t done = newLabel();
            emit(MIR_DGT, reg(cond), reg(x), MIR_new_double_op(ctx_, 0.0));
            emit(MIR_DMOV, reg(r), MIR_new_double_op(ctx_, 1.0));
            emit(MIR_BT, label(done), reg(cond));
            emit(MIR_DLT, reg(cond), reg(x), MIR_new_double_op(ctx_, 0.0));
            emit(MIR_DMOV, reg(r), MIR_new_double_op(ctx_, -1.0));
            emit(MIR_BT, label(done), reg(cond));
            emit(MIR_DMOV, reg(r), MIR_new_double_op(ctx_, 0.0));
            place(done);
            return reg(r);
        }
        case Intrinsic::IsFinite: {
            // x - x == 0.0
            MIR_reg_t x = toReg(args(0), VK::D);
            MIR_reg_t d = temp(VK::D), r = temp(VK::I);
            emit(MIR_DSUB, reg(d), reg(x), reg(x));
            emit(MIR_DEQ, reg(r), reg(d), MIR_new_double_op(ctx_, 0.0));
            return reg(r);
        }
        default:
            break;
    }
    // Methods whose object is c->object (or this).
    auto objectAddress = [&]() -> MIR_reg_t {
        if (c->object) return aggregate(c->object);
        return selfReg_;
    };
    switch (which) {
        case Intrinsic::ParamValue: {
            MIR_reg_t p = objectAddress();
            MIR_reg_t r = temp(VK::D);
            emit(MIR_DMOV, reg(r), mem(MIR_T_D, p, D::Layout::paramValue));
            return reg(r);
        }
        case Intrinsic::ParamAt: {
            MIR_reg_t p = objectAddress();
            MIR_op_t i = args(0);
            MIR_op_t channel = c->args.size() == 2 ? args(1) : imm(0);
            MIR_reg_t r = temp(VK::D);
            callC(reinterpret_cast<void*>(&supple_script_param_at),
                  {Abi::F64, Abi::Ptr, Abi::I32, Abi::I32}, {reg(p), i, channel}, r);
            return reg(r);
        }
        case Intrinsic::ParamIndex: {
            MIR_reg_t p = objectAddress();
            MIR_reg_t v = temp(VK::D), r = temp(VK::I);
            emit(MIR_DMOV, reg(v), mem(MIR_T_D, p, D::Layout::paramValue));
            callC(reinterpret_cast<void*>(&supple_script_round), {Abi::I32, Abi::F64}, {reg(v)}, r);
            return reg(r);
        }
        case Intrinsic::ParamOn: {
            MIR_reg_t p = objectAddress();
            MIR_reg_t v = temp(VK::D), r = temp(VK::I);
            emit(MIR_DMOV, reg(v), mem(MIR_T_D, p, D::Layout::paramValue));
            emit(MIR_DGT, reg(r), reg(v), MIR_new_double_op(ctx_, 0.5));
            return reg(r);
        }
        case Intrinsic::ParamChanged: {
            MIR_reg_t p = objectAddress();
            MIR_reg_t r = temp(VK::I);
            emit(MIR_MOV, reg(r), mem(MIR_T_U8, p, D::Layout::paramChanged));
            return reg(r);
        }
        case Intrinsic::ParamSet: {
            MIR_op_t v = args(0);
            MIR_reg_t p = objectAddress();
            emit(MIR_DMOV, mem(MIR_T_D, p, D::Layout::paramReported), v);
            return imm(0);
        }
        case Intrinsic::SharedGet:
            // Shared<T>: its T, at the start of the member.
            return load(t, Place{false, objectAddress(), 0});
        case Intrinsic::SharedSet: {
            const Type* vt = c->builtinMethod->owner->sharedValue;
            MIR_op_t v = args(0);
            store(vt, Place{false, objectAddress(), 0}, v);
            return imm(0);
        }
        case Intrinsic::SampleRate:
        case Intrinsic::MaxBlockSize:
        case Intrinsic::Channels:
        case Intrinsic::Offline: {
            MIR_reg_t instance = ptemp();
            emit(MIR_MOV, reg(instance), mem(MIR_T_P, selfReg_, D::Layout::pluginInstance));
            if (which == Intrinsic::SampleRate) {
                MIR_reg_t r = temp(VK::D);
                emit(MIR_DMOV, reg(r), mem(MIR_T_D, instance, offsetof(D::Instance, sampleRate)));
                return reg(r);
            }
            MIR_reg_t r = temp(VK::I);
            if (which == Intrinsic::MaxBlockSize)
                emit(MIR_MOV, reg(r),
                     mem(MIR_T_I32, instance, offsetof(D::Instance, maxBlockSize)));
            else if (which == Intrinsic::Channels)
                emit(MIR_MOV, reg(r), mem(MIR_T_I32, instance, offsetof(D::Instance, channels)));
            else
                emit(MIR_MOV, reg(r), mem(MIR_T_U8, instance, offsetof(D::Instance, offline)));
            return reg(r);
        }
        default:
            break;
    }
    diags_.error(c->loc, "script.internal", "internal error: an intrinsic without code",
                 "please report it");
    return imm(0);
}

MIR_op_t Codegen::call(CallExpr* c) {
    const Type* t = c->type();
    // A script function or member function.
    if (c->function) {
        FunctionDecl* f = c->function;
        callees_[fn_].insert(f);
        std::vector<MIR_op_t> ops{MIR_new_ref_op(ctx_, functionProtos_[f]),
                                  MIR_new_ref_op(ctx_, forwards_[f])};
        MIR_reg_t result = 0;
        if (t && !t->isVoid()) {
            result = temp(kindOf(t));
            ops.push_back(reg(result));
        }
        ops.push_back(reg(ctxReg_));
        if (f->owner) {
            MIR_reg_t self = c->object ? aggregate(c->object) : selfReg_;
            if (c->object && c->object->kind != ExprKind::This) {
                // keep it
            }
            ops.push_back(reg(self));
        }
        for (size_t i = 0; i < f->params.size(); ++i) {
            VarDecl* p = f->params[i];
            Expr* argument = i < c->args.size() ? c->args[i] : p->init;
            if (p->qt.isRef) {
                if (argument->lvalue && argument->type() == p->qt.type) {
                    ops.push_back(reg(addressOf(placeOf(argument))));
                } else {
                    // A const reference to a temporary number.
                    int off = 0;
                    MIR_reg_t slot = frameSlot(p->qt.type->size, p->qt.type->align, off);
                    store(p->qt.type, Place{false, slot, 0}, value(argument));
                    ops.push_back(reg(slot));
                }
            } else {
                MIR_op_t v = value(argument);
                // Fix the value now: a later argument must not change it.
                ops.push_back(reg(toReg(v, kindOf(p->qt.type))));
                if (argument->lvalue && v.mode == MIR_OP_REG) {
                    MIR_reg_t copy = temp(kindOf(p->qt.type));
                    move(kindOf(p->qt.type), reg(copy), v);
                    ops.back() = reg(copy);
                }
            }
        }
        emit(MIR_new_insn_arr(ctx_, MIR_CALL, ops.size(), ops.data()));
        abortCheck();
        return result ? reg(result) : imm(0);
    }
    if (c->builtin) {
        const BuiltinFunction* bf = c->builtin;
        if (bf->intrinsic != Intrinsic::None) return intrinsic(c, bf->intrinsic);
        std::vector<MIR_op_t> args;
        for (size_t i = 0; i < c->args.size(); ++i) {
            MIR_op_t v = value(c->args[i]);
            if (c->args[i]->lvalue && v.mode == MIR_OP_REG) {
                MIR_reg_t copy = temp(kindOf(c->args[i]->type()));
                move(kindOf(c->args[i]->type()), reg(copy), v);
                v = reg(copy);
            }
            args.push_back(v);
        }
        MIR_reg_t result = 0;
        if (bf->abi[0] != Abi::Void) result = temp(kindOf(t));
        callC(bf->thunk, bf->abi, args, result);
        if (bf->abi[0] == Abi::I32 && t->kind == TypeKind::Bool)
            emit(MIR_NE, reg(result), reg(result), imm(0));
        return result ? reg(result) : imm(0);
    }
    if (c->builtinMethod) {
        const BuiltinMethod* bm = c->builtinMethod;
        if (bm->intrinsic != Intrinsic::None) return intrinsic(c, bm->intrinsic);
        if (bm->owner == b_.channel() || bm->owner == b_.inputChannel()) {
            // size()
            MIR_reg_t data = 0, frames = 0, spare = 0;
            channelOf(c->object, data, frames, spare);
            return reg(frames);
        }
        MIR_reg_t self = c->object ? aggregate(c->object) : selfReg_;
        std::vector<MIR_op_t> args{reg(self)};
        for (size_t i = 0; i < c->args.size(); ++i) {
            // An object by reference (the Editor's Param and file
            // arguments): its address.
            if (i < bm->params.size() && bm->params[i].isRef) {
                args.push_back(reg(aggregate(c->args[i])));
                continue;
            }
            MIR_op_t v = value(c->args[i]);
            if (c->args[i]->lvalue && v.mode == MIR_OP_REG) {
                MIR_reg_t copy = temp(kindOf(c->args[i]->type()));
                move(kindOf(c->args[i]->type()), reg(copy), v);
                v = reg(copy);
            }
            args.push_back(v);
        }
        if (bm->owner == b_.param() && bm->name == "gain") {
            int off = 0;
            MIR_reg_t out = frameSlot(8, 8, off);
            callC(reinterpret_cast<void*>(&supple_script_param_gain),
                  {Abi::Void, Abi::Ptr, Abi::Ptr}, {reg(self), reg(out)});
            MIR_reg_t r = temp(VK::D);
            emit(MIR_DMOV, reg(r), mem(MIR_T_D, out, 0));
            return reg(r);
        }
        MIR_reg_t result = 0;
        if (bm->abi[0] != Abi::Void) result = temp(kindOf(t));
        callC(bm->thunk, bm->abi, args, result);
        if (bm->abi[0] == Abi::I32 && t->kind == TypeKind::Bool)
            emit(MIR_NE, reg(result), reg(result), imm(0));
        return result ? reg(result) : imm(0);
    }
    // Array and Buffer members.
    if (c->callee->kind == ExprKind::Member) {
        const std::string& m = static_cast<MemberExpr*>(c->callee)->member;
        const Type* ot = c->object->type();
        if (m == "size") {
            MIR_reg_t object = aggregate(c->object);
            if (ot->kind == TypeKind::Array) return imm(ot->count);
            MIR_reg_t r = temp(VK::I);
            const int sizeOff =
                ot->element->kind == TypeKind::Double  ? D::Layout::bufferSize<double>()
                : ot->element->kind == TypeKind::Float ? D::Layout::bufferSize<float>()
                                                       : D::Layout::bufferSize<int>();
            emit(MIR_MOV, reg(r), mem(MIR_T_I32, object, sizeOff));
            return reg(r);
        }
        if (m == "resize") {
            MIR_op_t n = value(c->args[0]);
            MIR_reg_t object = aggregate(c->object);
            void* fn = ot->element->kind == TypeKind::Double
                           ? reinterpret_cast<void*>(&supple_script_resize_d)
                       : ot->element->kind == TypeKind::Float
                           ? reinterpret_cast<void*>(&supple_script_resize_f)
                           : reinterpret_cast<void*>(&supple_script_resize_i);
            callC(fn, {Abi::Void, Abi::Ptr, Abi::I32}, {reg(object), n});
            return imm(0);
        }
        if (m == "fill") {
            const Type* el = ot->element;
            MIR_reg_t object = aggregate(c->object);
            MIR_reg_t base = ptemp(), count = ptemp();
            if (ot->kind == TypeKind::Array) {
                emit(MIR_MOV, reg(base), reg(object));
                emit(MIR_MOV, reg(count), imm(ot->count));
            } else {
                const int itemsOff = el->kind == TypeKind::Double ? D::Layout::bufferItems<double>()
                                     : el->kind == TypeKind::Float ? D::Layout::bufferItems<float>()
                                                                   : D::Layout::bufferItems<int>();
                const int sizeOff = el->kind == TypeKind::Double  ? D::Layout::bufferSize<double>()
                                    : el->kind == TypeKind::Float ? D::Layout::bufferSize<float>()
                                                                  : D::Layout::bufferSize<int>();
                emit(MIR_MOV, reg(base), mem(MIR_T_P, object, itemsOff));
                emit(MIR_MOV, reg(count), mem(MIR_T_I32, object, sizeOff));
            }
            MIR_op_t v = el->isScalar() ? value(c->args[0]) : imm(0);
            MIR_reg_t src = el->isScalar() ? 0 : aggregate(c->args[0]);
            MIR_reg_t i = ptemp(), at = ptemp();
            MIR_label_t top = newLabel(), done = newLabel();
            emit(MIR_MOV, reg(i), imm(0));
            place(top);
            emit(MIR_BGE, label(done), reg(i), reg(count));
            emit(MIR_MUL, reg(at), reg(i), imm(el->size));
            emit(MIR_ADD, reg(at), reg(at), reg(base));
            if (el->isScalar())
                store(el, Place{false, at, 0}, v);
            else
                copyBytes(at, src, el->size);
            emit(MIR_ADD, reg(i), reg(i), imm(1));
            emit(MIR_JMP, label(top));
            place(done);
            return imm(0);
        }
    }
    diags_.error(c->loc, "script.internal", "internal error: a call without code",
                 "please report it");
    return imm(0);
}

// --- statements -------------------------------------------------------------------------------

void Codegen::budgetCheck(int line) {
    MIR_reg_t left = ptemp();
    MIR_label_t fine = newLabel();
    emit(MIR_MOV, reg(left), mem(MIR_T_I64, ctxReg_, kBudgetOffset));
    emit(MIR_SUB, reg(left), reg(left), imm(1));
    emit(MIR_MOV, mem(MIR_T_I64, ctxReg_, kBudgetOffset), reg(left));
    emit(MIR_BGE, label(fine), reg(left), imm(0));
    callC(reinterpret_cast<void*>(&supple_script_budget), {Abi::Void, Abi::Ptr, Abi::I32},
          {reg(ctxReg_), imm(line)});
    emit(MIR_JMP, label(exit_));
    place(fine);
}

void Codegen::abortCheck() {
    MIR_reg_t left = ptemp();
    emit(MIR_MOV, reg(left), mem(MIR_T_I64, ctxReg_, kBudgetOffset));
    emit(MIR_BLT, label(exit_), reg(left), imm(0));
}

void Codegen::statement(Stmt* s) {
    if (!s) return;
    switch (s->kind) {
        case StmtKind::Compound:
            for (Stmt* x : static_cast<CompoundStmt*>(s)->body) statement(x);
            return;
        case StmtKind::Empty:
            return;
        case StmtKind::Expr: {
            Expr* e = static_cast<ExprStmt*>(s)->expr;
            if (e->type() && e->type()->isScalar())
                value(e);
            else if (e->kind == ExprKind::Call)
                call(static_cast<CallExpr*>(e));
            else if (e->kind == ExprKind::Assign)
                value(e);
            else if (e->kind == ExprKind::Binary)
                value(e);
            else
                value(e);
            return;
        }
        case StmtKind::Decl:
            for (VarDecl* v : static_cast<DeclStmt*>(s)->vars) {
                Local& l = locals_[v];
                const Type* t = v->qt.type;
                if (v->qt.isRef) {
                    l.reference = true;
                    l.reg = ptemp();
                    if (v->init->lvalue && v->init->type() == t) {
                        emit(MIR_MOV, reg(l.reg), reg(addressOf(placeOf(v->init))));
                    } else {
                        int off = 0;
                        MIR_reg_t slot = frameSlot(t->size, t->align, off);
                        initialize(slot, t, v->init, v->loc.line);
                        emit(MIR_MOV, reg(l.reg), reg(slot));
                    }
                    continue;
                }
                if ((t->isScalar() || t->kind == TypeKind::String) && !v->addressTaken) {
                    l.inReg = true;
                    l.reg = temp(kindOf(t));
                    move(kindOf(t), reg(l.reg), v->init ? value(v->init) : zero(kindOf(t)));
                    continue;
                }
                int off = 0;
                MIR_reg_t addr = frameSlot(t->size, t->align, off);
                l.frame = off;
                initialize(addr, t, v->init, v->loc.line);
            }
            return;
        case StmtKind::If: {
            auto* i = static_cast<IfStmt*>(s);
            MIR_label_t otherwise = newLabel(), done = newLabel();
            emit(MIR_BF, label(otherwise), value(i->cond));
            statement(i->then);
            emit(MIR_JMP, label(done));
            place(otherwise);
            statement(i->otherwise);
            place(done);
            return;
        }
        case StmtKind::While: {
            auto* w = static_cast<WhileStmt*>(s);
            MIR_label_t top = newLabel(), next = newLabel(), done = newLabel();
            place(top);
            emit(MIR_BF, label(done), value(w->cond));
            loops_.push_back({done, next});
            breaks_.push_back(done);
            statement(w->body);
            breaks_.pop_back();
            loops_.pop_back();
            place(next);
            budgetCheck(w->loc.line);
            emit(MIR_JMP, label(top));
            place(done);
            return;
        }
        case StmtKind::DoWhile: {
            auto* d = static_cast<DoWhileStmt*>(s);
            MIR_label_t top = newLabel(), next = newLabel(), done = newLabel();
            place(top);
            loops_.push_back({done, next});
            breaks_.push_back(done);
            statement(d->body);
            breaks_.pop_back();
            loops_.pop_back();
            place(next);
            MIR_op_t c = value(d->cond);
            MIR_reg_t cr = toReg(c, VK::I);
            emit(MIR_BF, label(done), reg(cr));
            budgetCheck(d->loc.line);
            emit(MIR_JMP, label(top));
            place(done);
            return;
        }
        case StmtKind::For: {
            auto* f = static_cast<ForStmt*>(s);
            statement(f->init);
            MIR_label_t top = newLabel(), next = newLabel(), done = newLabel();
            place(top);
            if (f->cond) emit(MIR_BF, label(done), value(f->cond));
            loops_.push_back({done, next});
            breaks_.push_back(done);
            statement(f->body);
            breaks_.pop_back();
            loops_.pop_back();
            place(next);
            if (f->step) value(f->step);
            budgetCheck(f->loc.line);
            emit(MIR_JMP, label(top));
            place(done);
            return;
        }
        case StmtKind::RangeFor: {
            auto* r = static_cast<RangeForStmt*>(s);
            const Type* rt = r->range->type();
            const Type* el = rt->element;
            MIR_reg_t object = aggregate(r->range);
            MIR_reg_t base = ptemp(), count = ptemp(), i = ptemp();
            if (rt->kind == TypeKind::Array) {
                emit(MIR_MOV, reg(base), reg(object));
                emit(MIR_MOV, reg(count), imm(rt->count));
            } else {
                const int itemsOff = el->kind == TypeKind::Double ? D::Layout::bufferItems<double>()
                                     : el->kind == TypeKind::Float ? D::Layout::bufferItems<float>()
                                                                   : D::Layout::bufferItems<int>();
                const int sizeOff = el->kind == TypeKind::Double  ? D::Layout::bufferSize<double>()
                                    : el->kind == TypeKind::Float ? D::Layout::bufferSize<float>()
                                                                  : D::Layout::bufferSize<int>();
                emit(MIR_MOV, reg(base), mem(MIR_T_P, object, itemsOff));
                emit(MIR_MOV, reg(count), mem(MIR_T_I32, object, sizeOff));
            }
            MIR_label_t top = newLabel(), next = newLabel(), done = newLabel();
            emit(MIR_MOV, reg(i), imm(0));
            place(top);
            emit(MIR_BGE, label(done), reg(i), reg(count));
            MIR_reg_t at = ptemp();
            emit(MIR_MUL, reg(at), reg(i), imm(el->size));
            emit(MIR_ADD, reg(at), reg(at), reg(base));
            VarDecl* v = r->var;
            Local& l = locals_[v];
            if (v->qt.isRef) {
                l.reference = true;
                l.reg = at;
            } else if (v->qt.type->isScalar() && !v->addressTaken) {
                l.inReg = true;
                l.reg = temp(kindOf(v->qt.type));
                move(kindOf(v->qt.type), reg(l.reg),
                     convertValue(load(el, Place{false, at, 0}), el, v->qt.type));
            } else {
                int off = 0;
                MIR_reg_t slot = frameSlot(v->qt.type->size, v->qt.type->align, off);
                l.frame = off;
                if (v->qt.type->isScalar())
                    store(v->qt.type, Place{false, slot, 0},
                          convertValue(load(el, Place{false, at, 0}), el, v->qt.type));
                else
                    copyBytes(slot, at, el->size);
            }
            loops_.push_back({done, next});
            breaks_.push_back(done);
            statement(r->body);
            breaks_.pop_back();
            loops_.pop_back();
            place(next);
            emit(MIR_ADD, reg(i), reg(i), imm(1));
            budgetCheck(r->loc.line);
            emit(MIR_JMP, label(top));
            place(done);
            return;
        }
        case StmtKind::Switch: {
            auto* sw = static_cast<SwitchStmt*>(s);
            MIR_reg_t v = toReg(value(sw->cond), VK::I);
            if (sw->cond->lvalue) {
                MIR_reg_t copy = temp(VK::I);
                emit(MIR_MOV, reg(copy), reg(v));
                v = copy;
            }
            MIR_label_t done = newLabel();
            std::map<CaseStmt*, MIR_label_t> labels;
            MIR_label_t defaultLabel = done;
            for (Stmt* st : sw->body->body)
                if (st->kind == StmtKind::Case) {
                    auto* c = static_cast<CaseStmt*>(st);
                    labels[c] = newLabel();
                    if (!c->value) defaultLabel = labels[c];
                }
            for (auto& [c, l] : labels)
                if (c->value) emit(MIR_BEQ, label(l), reg(v), imm(c->constValue));
            emit(MIR_JMP, label(defaultLabel));
            breaks_.push_back(done);
            for (Stmt* st : sw->body->body) {
                if (st->kind == StmtKind::Case) {
                    place(labels[static_cast<CaseStmt*>(st)]);
                    continue;
                }
                statement(st);
            }
            breaks_.pop_back();
            place(done);
            return;
        }
        case StmtKind::Case:
            return;
        case StmtKind::Break:
            emit(MIR_JMP, label(breaks_.back()));
            return;
        case StmtKind::Continue:
            emit(MIR_JMP, label(loops_.back().second));
            return;
        case StmtKind::Return: {
            auto* r = static_cast<ReturnStmt*>(s);
            if (r->value && fn_->resultType.type && !fn_->resultType.type->isVoid()) {
                MIR_op_t v = value(r->value);
                emit(MIR_new_ret_insn(ctx_, 1, v));
            } else {
                if (r->value) value(r->value);
                emit(MIR_new_ret_insn(ctx_, 0));
            }
            return;
        }
    }
}

// --- functions --------------------------------------------------------------------------------

void Codegen::function(FunctionDecl* f) {
    fn_ = f;
    temps_ = 0;
    frameSize_ = 0;
    locals_.clear();
    loops_.clear();
    breaks_.clear();
    std::vector<MIR_var_t> vars;
    names_.push_back(std::make_unique<std::vector<std::string>>());
    auto& owned = *names_.back();
    owned.reserve(f->params.size() + 2);
    owned.push_back("ctx");
    vars.push_back(MIR_var_t{MIR_T_P, owned.back().c_str(), 0});
    if (f->owner) {
        owned.push_back("self");
        vars.push_back(MIR_var_t{MIR_T_P, owned.back().c_str(), 0});
    }
    for (VarDecl* p : f->params) {
        owned.push_back("p_" + p->name);
        const MIR_type_t t = p->qt.isRef ? MIR_T_P : regType(kindOf(p->qt.type));
        vars.push_back(MIR_var_t{t, owned.back().c_str(), 0});
    }
    const Type* rt = f->resultType.type;
    MIR_type_t res = rt && !rt->isVoid() ? regType(kindOf(rt)) : MIR_T_I64;
    const std::string& name = functionNames_[f];
    func_ = MIR_new_func_arr(ctx_, name.c_str(), rt && !rt->isVoid() ? 1 : 0, &res, vars.size(),
                             vars.data());
    items_[f] = func_;
    ctxReg_ = MIR_reg(ctx_, "ctx", func_->u.func);
    selfReg_ = f->owner ? MIR_reg(ctx_, "self", func_->u.func) : 0;
    frameReg_ = ptemp();
    exit_ = newLabel();
    // Parameters: registers, or memory when something refers to them.
    for (VarDecl* p : f->params) {
        Local& l = locals_[p];
        MIR_reg_t r = MIR_reg(ctx_, ("p_" + p->name).c_str(), func_->u.func);
        if (p->qt.isRef) {
            l.reference = true;
            l.reg = r;
        } else if (p->addressTaken) {
            int off = 0;
            MIR_reg_t slot = frameSlot(p->qt.type->size, p->qt.type->align, off);
            l.frame = off;
            store(p->qt.type, Place{false, slot, 0}, reg(r));
        } else {
            l.inReg = true;
            l.reg = r;
        }
    }
    // A call spends the budget as a loop's turn does: a tree of calls with no
    // loop in it can still do exponential work.
    budgetCheck(f->loc.line);
    statement(f->body);
    // Falling off the end (void), or the abort exit: return zero.
    place(exit_);
    if (rt && !rt->isVoid())
        emit(MIR_new_ret_insn(ctx_, 1, zero(kindOf(rt))));
    else
        emit(MIR_new_ret_insn(ctx_, 0));
    // The frame, now that its size is known, allocated first thing.
    MIR_prepend_insn(ctx_, func_,
                     MIR_new_insn(ctx_, MIR_ALLOCA, reg(frameReg_),
                                  imm(std::max(16, (frameSize_ + 15) / 16 * 16))));
    MIR_finish_func(ctx_);
    frames_[f] = frameSize_;
}

// The frames of the deepest chain of calls from f (recursion is refused,
// so the calls form no cycle).
int Codegen::stackDepth(const FunctionDecl* f, std::map<const FunctionDecl*, int>& known) const {
    if (auto it = known.find(f); it != known.end()) return it->second;
    int deepest = 0;
    if (auto it = callees_.find(f); it != callees_.end())
        for (const FunctionDecl* g : it->second) deepest = std::max(deepest, stackDepth(g, known));
    const auto frame = frames_.find(f);
    return known[f] = deepest + (frame == frames_.end() ? 0 : frame->second);
}

std::shared_ptr<const Program> Codegen::run() {
    auto program = std::shared_ptr<Program>(new Program());
    program_ = program->d_.get();
    PluginModel& model = an_.plugin;
    ProgramData& d = *program->d_;
    d.info = model.info;
    d.moduleInfo = model.moduleInfo;
    d.ports = model.ports;
    d.typeName = model.decl ? model.decl->name : std::string();
    d.kind = model.kind;
    d.params = model.params;
    d.factoryPatches = model.factoryPatches;
    d.files = model.files;
    d.sources = model.sources;
    d.shared = model.shared;
    d.size = model.size;
    d.align = model.align;
    d.objects = model.objects;
    d.initial = model.initial;
    d.paramSlots = model.paramSlots;
    d.memberSpares = model.memberSpares;
    d.sourceSlots = model.sourceSlots;
    d.interpreted = options_.interpret;

    std::jmp_buf jump;
    tMirJump = &jump;
    ctx_ = MIR_init();
    d.mir = ctx_;
    if (setjmp(jump)) {
        diags_.error(Loc{}, "script.internal",
                     std::string("internal error in code generation: ") + tMirError,
                     "please report it, with the script");
        tMirJump = nullptr;
        return nullptr;
    }
    MIR_set_error_func(ctx_, mirError);
    module_ = MIR_new_module(ctx_, "script");
    // Every function gets a name and a forward declaration, so calls in
    // either direction resolve.
    int n = 0;
    for (FunctionDecl* f : an_.functions) {
        functionNames_[f] = "f" + std::to_string(n++) + "_" + f->name;
        forwards_[f] = MIR_new_forward(ctx_, functionNames_[f].c_str());
        std::vector<Abi> abi;
        const Type* rt = f->resultType.type;
        abi.push_back(!rt || rt->isVoid()   ? Abi::Void
                      : kindOf(rt) == VK::D ? Abi::F64
                      : kindOf(rt) == VK::F ? Abi::F32
                                            : Abi::I64);
        abi.push_back(Abi::Ptr);
        if (f->owner) abi.push_back(Abi::Ptr);
        for (VarDecl* p : f->params)
            abi.push_back(p->qt.isRef                   ? Abi::Ptr
                          : kindOf(p->qt.type) == VK::D ? Abi::F64
                          : kindOf(p->qt.type) == VK::F ? Abi::F32
                                                        : Abi::I64);
        functionProtos_[f] = proto(abi);
    }
    for (FunctionDecl* f : an_.functions) function(f);
    std::map<const FunctionDecl*, int> depths;
    for (FunctionDecl* f : an_.functions)
        if (stackDepth(f, depths) > kMaxStackBytes) {
            diags_.error(f->loc, "script.stack",
                         f->name + "() and the functions it calls need more than " +
                             std::to_string(kMaxStackBytes >> 10) +
                             " KiB of local storage together",
                         "move large arrays or structs to plugin members, or call fewer "
                         "functions with large locals from one another");
            break;
        }
    MIR_finish_module(ctx_);
    if (diags_.hasErrors()) {
        tMirJump = nullptr;
        return nullptr;
    }
    if (options_.listing) {
#ifdef _WIN32
        // ISO C tmpfile is binary on Windows; this optional listing is made
        // during compilation, never on the audio thread.
        FILE* f = std::tmpfile();
        if (f) {
            MIR_output_module(ctx_, f, module_);
            std::rewind(f);
            char block[4096];
            while (const size_t count = std::fread(block, 1, sizeof(block), f))
                d.listing.append(block, count);
            std::fclose(f);
        }
#else
        char* text = nullptr;
        size_t size = 0;
        FILE* f = open_memstream(&text, &size);
        if (f) {
            MIR_output_module(ctx_, f, module_);
            std::fclose(f);
            d.listing.assign(text, size);
            std::free(text);
        }
#endif
    }
    MIR_load_module(ctx_, module_);
    tImports = &imports_;
    if (options_.interpret) {
        MIR_link(ctx_, MIR_set_interp_interface, resolveImport);
    } else {
        MIR_gen_init(ctx_);
        d.generator = true;
        MIR_gen_set_optimize_level(ctx_,
                                   static_cast<unsigned>(std::clamp(options_.optimize, 0, 3)));
        MIR_link(ctx_, MIR_set_gen_interface, resolveImport);
    }
    tImports = nullptr;
    tMirJump = nullptr;
    auto hook = [&](FunctionDecl* f) -> void* {
        auto it = f ? items_.find(f) : items_.end();
        return it == items_.end() ? nullptr : it->second->addr;
    };
    d.prepare = reinterpret_cast<Program::Hook>(hook(model.prepare));
    d.update = reinterpret_cast<Program::Hook>(hook(model.update));
    if (model.kind == PluginKind::Module)
        d.moduleProcess = reinterpret_cast<D::ModuleCode::ProcessHook>(hook(model.process));
    else
        d.process = reinterpret_cast<Program::ProcessHook>(hook(model.process));
    d.startVoice = reinterpret_cast<Program::VoiceHook>(hook(model.startVoice));
    d.startVoiceRetrigger = reinterpret_cast<Program::StartHook>(hook(model.startVoiceRetrigger));
    d.releaseVoice = reinterpret_cast<Program::VoiceHook>(hook(model.releaseVoice));
    d.stopVoice = reinterpret_cast<Program::VoiceHook>(hook(model.stopVoice));
    d.editor = reinterpret_cast<Program::UiHook>(hook(model.editor));
    d.draw = reinterpret_cast<Program::UiHook>(hook(model.draw));
    d.pointer = reinterpret_cast<Program::UiHook>(hook(model.pointer));
    program_ = nullptr;
    return program;
}

std::shared_ptr<const Program> generate(Unit& unit, Analysis& analysis,
                                        const CompileOptions& options, Diagnostics& diags) {
    (void)unit;  // the tree lives there; the analysis points into it
    return Codegen(analysis, options, diags).run();
}

// --- instances ---------------------------------------------------------------------------------

Program::Program() : d_(std::make_unique<ProgramData>()) {}

Program::~Program() {
    if (d_ && d_->mir) {
        auto* ctx = static_cast<MIR_context_t>(d_->mir);
        if (d_->generator) MIR_gen_finish(ctx);
        MIR_finish(ctx);
    }
}

const InfoModel& Program::info() const { return d_->info; }
const std::string& Program::typeName() const { return d_->typeName; }
PluginKind Program::kind() const { return d_->kind; }
const std::vector<ParamModel>& Program::params() const { return d_->params; }
const std::vector<FactoryPatchModel>& Program::factoryPatches() const { return d_->factoryPatches; }
const std::vector<FileModel>& Program::files() const { return d_->files; }
const std::vector<SourceModel>& Program::sources() const { return d_->sources; }
const std::vector<SharedModel>& Program::shared() const { return d_->shared; }
bool Program::interpreted() const { return d_->interpreted; }
const std::string& Program::listing() const { return d_->listing; }
std::size_t Program::size() const { return static_cast<std::size_t>(d_->size); }
Program::Hook Program::prepare() const { return d_->prepare; }
Program::Hook Program::update() const { return d_->update; }
Program::ProcessHook Program::process() const { return d_->process; }
Program::VoiceHook Program::startVoice() const { return d_->startVoice; }
Program::StartHook Program::startVoiceRetrigger() const { return d_->startVoiceRetrigger; }
Program::VoiceHook Program::releaseVoice() const { return d_->releaseVoice; }
Program::VoiceHook Program::stopVoice() const { return d_->stopVoice; }
Program::UiHook Program::editor() const {
    return d_->kind == PluginKind::Module ? nullptr : d_->editor;
}
Program::UiHook Program::panel() const {
    return d_->kind == PluginKind::Module ? d_->editor : nullptr;
}
const ModuleInfoModel& Program::moduleInfo() const { return d_->moduleInfo; }
const std::vector<PortModel>& Program::ports() const { return d_->ports; }
D::ModuleCode::ProcessHook Program::moduleProcess() const { return d_->moduleProcess; }
Program::UiHook Program::draw() const { return d_->draw; }
Program::UiHook Program::pointer() const { return d_->pointer; }

void* Program::create() const {
    const std::size_t bytes = (static_cast<std::size_t>(d_->size) + 63) / 64 * 64;
    void* self = ::operator new(bytes, std::align_val_t{64});
    std::memset(self, 0, bytes);
    auto* base = static_cast<unsigned char*>(self);
    for (const InitialValue& v : d_->initial) {
        unsigned char* at = base + v.offset;
        switch (v.kind) {
            case TypeKind::Bool:
                *at = v.value != 0.0;
                break;
            case TypeKind::Int:
            case TypeKind::Enum: {
                const auto x = static_cast<std::int32_t>(v.value);
                std::memcpy(at, &x, 4);
                break;
            }
            case TypeKind::Float: {
                const auto x = static_cast<float>(v.value);
                std::memcpy(at, &x, 4);
                break;
            }
            default:
                std::memcpy(at, &v.value, 8);
                break;
        }
    }
    for (const ObjectSlot& o : d_->objects) o.cls->construct(base + o.offset);
    for (const ParamSlot& p : d_->paramSlots) {
        S::ParamSpec spec;
        spec.def = p.def;
        spec.min = p.min;
        spec.offAtMinimum = p.offAtMinimum;
        auto* param = new (base + p.offset) S::Param(spec);
        D::ParamBinding::setSlot(*param, p.index);
    }
    for (const SourceSlot& s : d_->sourceSlots) {
        S::SourceSpec spec;
        spec.perVoice = s.perVoice;
        spec.bipolar = s.bipolar;
        new (base + s.offset) S::Source(spec);
    }
    for (const PortModel& port : d_->ports) {
        S::PortSpec spec;
        spec.output = port.output;
        spec.signal = static_cast<S::SignalKind>(port.signal);
        spec.normalValue = port.normal;
        if (port.output) {
            auto* out = new (base + port.offset) S::Output(spec);
            D::PortBinding::setSlot(*out, port.index);
        } else {
            auto* in = new (base + port.offset) S::Input(spec);
            D::PortBinding::setSlot(*in, port.index);
        }
    }
    for (const MemberSpare& slot : d_->memberSpares) {
        double* sample = reinterpret_cast<double*>(base + slot.offset + slot.cls->type->size);
        if (slot.cls == Builtins::get().param()) {
            auto* p =
                new (base + slot.offset) S::Param(*reinterpret_cast<S::Param*>(base + slot.first));
            D::resetMember(*p, sample);
        } else if (slot.cls == Builtins::get().cls("Input")) {
            auto* p =
                new (base + slot.offset) S::Input(*reinterpret_cast<S::Input*>(base + slot.first));
            D::resetMember(*p, sample);
        } else {
            auto* p = new (base + slot.offset)
                S::Output(*reinterpret_cast<S::Output*>(base + slot.first));
            D::resetMember(*p, sample);
        }
    }
    return self;
}

void Program::destroy(void* self) const {
    if (!self) return;
    auto* base = static_cast<unsigned char*>(self);
    for (const ObjectSlot& o : d_->objects) o.cls->destroy(base + o.offset);
    ::operator delete(self, std::align_val_t{64});
}

void Program::copy(void* dst, const void* src) const {
    auto* d = static_cast<unsigned char*>(dst);
    const auto* s = static_cast<const unsigned char*>(src);
    // Plain bytes between the runtime objects; the objects through their own
    // copy (objects are in layout order).
    int at = 0;
    for (const ObjectSlot& o : d_->objects) {
        if (o.offset > at) std::memcpy(d + at, s + at, static_cast<std::size_t>(o.offset - at));
        o.cls->copy(d + o.offset, s + o.offset);
        at = o.offset + o.cls->type->size;
    }
    if (d_->size > at) std::memcpy(d + at, s + at, static_cast<std::size_t>(d_->size - at));
}

}  // namespace supple::scriptc
