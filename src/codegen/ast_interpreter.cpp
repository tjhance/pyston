// Copyright (c) 2014-2015 Dropbox, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "codegen/ast_interpreter.h"

#include <llvm/ADT/StringMap.h>
#include <unordered_map>

#include "analysis/function_analysis.h"
#include "analysis/scoping_analysis.h"
#include "codegen/codegen.h"
#include "codegen/compvars.h"
#include "codegen/irgen.h"
#include "codegen/irgen/hooks.h"
#include "codegen/irgen/irgenerator.h"
#include "codegen/irgen/util.h"
#include "codegen/osrentry.h"
#include "core/ast.h"
#include "core/cfg.h"
#include "core/common.h"
#include "core/stats.h"
#include "core/thread_utils.h"
#include "core/util.h"
#include "runtime/generator.h"
#include "runtime/import.h"
#include "runtime/inline/boxing.h"
#include "runtime/long.h"
#include "runtime/objmodel.h"
#include "runtime/set.h"
#include "runtime/types.h"

namespace pyston {

#define OSR_THRESHOLD 100
#define REOPT_THRESHOLD 10

union Value {
    bool b;
    int64_t n;
    double d;
    Box* o;

    Value(bool b) : b(b) {}
    Value(int64_t n = 0) : n(n) {}
    Value(double d) : d(d) {}
    Value(Box* o) : o(o) {}
};

class ASTInterpreter {
public:
    typedef llvm::StringMap<Box*> SymMap;

    ASTInterpreter(CompiledFunction* compiled_function);

    void initArguments(int nargs, BoxedClosure* closure, BoxedGenerator* generator, Box* arg1, Box* arg2, Box* arg3,
                       Box** args);
    static Value execute(ASTInterpreter& interpreter, AST_stmt* start_at = NULL);

private:
    Box* createFunction(AST* node, AST_arguments* args, const std::vector<AST_stmt*>& body);
    Value doBinOp(Box* left, Box* right, int op, BinExpType exp_type);
    void doStore(AST_expr* node, Value value);
    void doStore(const std::string& name, Value value);
    void eraseDeadSymbols();

    Value visit_assert(AST_Assert* node);
    Value visit_assign(AST_Assign* node);
    Value visit_binop(AST_BinOp* node);
    Value visit_call(AST_Call* node);
    Value visit_classDef(AST_ClassDef* node);
    Value visit_compare(AST_Compare* node);
    Value visit_delete(AST_Delete* node);
    Value visit_functionDef(AST_FunctionDef* node);
    Value visit_global(AST_Global* node);
    Value visit_module(AST_Module* node);
    Value visit_print(AST_Print* node);
    Value visit_raise(AST_Raise* node);
    Value visit_return(AST_Return* node);
    Value visit_stmt(AST_stmt* node);
    Value visit_unaryop(AST_UnaryOp* node);

    Value visit_attribute(AST_Attribute* node);
    Value visit_dict(AST_Dict* node);
    Value visit_expr(AST_expr* node);
    Value visit_expr(AST_Expr* node);
    Value visit_index(AST_Index* node);
    Value visit_lambda(AST_Lambda* node);
    Value visit_list(AST_List* node);
    Value visit_name(AST_Name* node);
    Value visit_num(AST_Num* node);
    Value visit_repr(AST_Repr* node);
    Value visit_set(AST_Set* node);
    Value visit_slice(AST_Slice* node);
    Value visit_str(AST_Str* node);
    Value visit_subscript(AST_Subscript* node);
    Value visit_tuple(AST_Tuple* node);
    Value visit_yield(AST_Yield* node);


    // pseudo
    Value visit_augBinOp(AST_AugBinOp* node);
    Value visit_branch(AST_Branch* node);
    Value visit_clsAttribute(AST_ClsAttribute* node);
    Value visit_invoke(AST_Invoke* node);
    Value visit_jump(AST_Jump* node);
    Value visit_langPrimitive(AST_LangPrimitive* node);

    CompiledFunction* compiled_func;
    SourceInfo* source_info;
    ScopeInfo* scope_info;

    SymMap sym_table;
    CFGBlock* next_block, *current_block;
    AST_stmt* current_inst;
    ExcInfo last_exception;
    BoxedClosure* passed_closure, *created_closure;
    BoxedGenerator* generator;
    unsigned edgecount;
    FrameInfo frame_info;

public:
    AST_stmt* getCurrentStatement() {
        assert(current_inst);
        return current_inst;
    }

    CompiledFunction* getCF() { return compiled_func; }
    FrameInfo* getFrameInfo() { return &frame_info; }
    const SymMap& getSymbolTable() { return sym_table; }
    void gcVisit(GCVisitor* visitor);
};

const void* interpreter_instr_addr = (void*)&ASTInterpreter::execute;

static_assert(THREADING_USE_GIL, "have to make the interpreter map thread safe!");
static std::unordered_map<void*, ASTInterpreter*> s_interpreterMap;

Box* astInterpretFunction(CompiledFunction* cf, int nargs, Box* closure, Box* generator, Box* arg1, Box* arg2,
                          Box* arg3, Box** args) {
    if (unlikely(cf->times_called > REOPT_THRESHOLD)) {
        CompiledFunction* optimized = reoptCompiledFuncInternal(cf);
        if (closure && generator)
            return optimized->closure_generator_call((BoxedClosure*)closure, (BoxedGenerator*)generator, arg1, arg2,
                                                     arg3, args);
        else if (closure)
            return optimized->closure_call((BoxedClosure*)closure, arg1, arg2, arg3, args);
        else if (generator)
            return optimized->generator_call((BoxedGenerator*)generator, arg1, arg2, arg3, args);
        return optimized->call(arg1, arg2, arg3, args);
    }

    ++cf->times_called;
    ASTInterpreter interpreter(cf);

    interpreter.initArguments(nargs, (BoxedClosure*)closure, (BoxedGenerator*)generator, arg1, arg2, arg3, args);
    Value v = ASTInterpreter::execute(interpreter);

    return v.o ? v.o : None;
}

Box* astInterpretFrom(CompiledFunction* cf, AST_stmt* start_at, BoxedDict* locals) {
    assert(locals->d.size() == 0);

    ASTInterpreter interpreter(cf);

    Value v = ASTInterpreter::execute(interpreter, start_at);

    return v.o ? v.o : None;
}

AST_stmt* getCurrentStatementForInterpretedFrame(void* frame_ptr) {
    ASTInterpreter* interpreter = s_interpreterMap[frame_ptr];
    assert(interpreter);
    return interpreter->getCurrentStatement();
}

CompiledFunction* getCFForInterpretedFrame(void* frame_ptr) {
    ASTInterpreter* interpreter = s_interpreterMap[frame_ptr];
    assert(interpreter);
    return interpreter->getCF();
}

FrameInfo* getFrameInfoForInterpretedFrame(void* frame_ptr) {
    ASTInterpreter* interpreter = s_interpreterMap[frame_ptr];
    assert(interpreter);
    return interpreter->getFrameInfo();
}

BoxedDict* localsForInterpretedFrame(void* frame_ptr, bool only_user_visible) {
    ASTInterpreter* interpreter = s_interpreterMap[frame_ptr];
    assert(interpreter);
    BoxedDict* rtn = new BoxedDict();
    for (auto&& l : interpreter->getSymbolTable()) {
        if (only_user_visible && (l.getKey()[0] == '!' || l.getKey()[0] == '#'))
            continue;

        rtn->d[new BoxedString(l.getKey())] = l.getValue();
    }
    return rtn;
}

void ASTInterpreter::gcVisit(GCVisitor* visitor) {
    for (const auto& p2 : getSymbolTable()) {
        visitor->visitPotential(p2.second);
    }

    if (passed_closure)
        visitor->visit(passed_closure);
    if (created_closure)
        visitor->visit(created_closure);
    if (generator)
        visitor->visit(generator);
}

void gatherInterpreterRoots(GCVisitor* visitor) {
    for (const auto& p : s_interpreterMap) {
        p.second->gcVisit(visitor);
    }
}


ASTInterpreter::ASTInterpreter(CompiledFunction* compiled_function)
    : compiled_func(compiled_function), source_info(compiled_function->clfunc->source), scope_info(0), next_block(0),
      current_block(0), current_inst(0), last_exception(NULL, NULL, NULL), passed_closure(0), created_closure(0),
      generator(0), edgecount(0), frame_info(ExcInfo(NULL, NULL, NULL)) {

    CLFunction* f = compiled_function->clfunc;
    if (!source_info->cfg)
        source_info->cfg = computeCFG(f->source, f->source->body);

    scope_info = source_info->getScopeInfo();
}

void ASTInterpreter::initArguments(int nargs, BoxedClosure* _closure, BoxedGenerator* _generator, Box* arg1, Box* arg2,
                                   Box* arg3, Box** args) {
    passed_closure = _closure;
    generator = _generator;

    if (scope_info->createsClosure())
        created_closure = createClosure(passed_closure);

    std::vector<Box*> argsArray{ arg1, arg2, arg3 };
    for (int i = 3; i < nargs; ++i)
        argsArray.push_back(args[i - 3]);

    const ParamNames& param_names = compiled_func->clfunc->param_names;

    int i = 0;
    for (const std::string& name : param_names.args) {
        doStore(name, argsArray[i++]);
    }

    if (!param_names.vararg.empty()) {
        doStore(param_names.vararg, argsArray[i++]);
    }

    if (!param_names.kwarg.empty()) {
        doStore(param_names.kwarg, argsArray[i++]);
    }
}

namespace {
class RegisterHelper {
private:
    void* frame_addr;

public:
    RegisterHelper(ASTInterpreter* interpreter, void* frame_addr) : frame_addr(frame_addr) {
        s_interpreterMap[frame_addr] = interpreter;
    }
    ~RegisterHelper() {
        assert(s_interpreterMap.count(frame_addr));
        s_interpreterMap.erase(frame_addr);
    }
};
}

Value ASTInterpreter::execute(ASTInterpreter& interpreter, AST_stmt* start_at) {
    threading::allowGLReadPreemption();

    assert(start_at == NULL);

    void* frame_addr = __builtin_frame_address(0);
    RegisterHelper frame_registerer(&interpreter, frame_addr);

    Value v;
    interpreter.next_block = interpreter.source_info->cfg->getStartingBlock();
    while (interpreter.next_block) {
        interpreter.current_block = interpreter.next_block;
        interpreter.next_block = 0;

        for (AST_stmt* s : interpreter.current_block->body) {
            interpreter.current_inst = s;
            v = interpreter.visit_stmt(s);
        }
    }
    return v;
}

void ASTInterpreter::eraseDeadSymbols() {
    if (source_info->liveness == NULL)
        source_info->liveness = computeLivenessInfo(source_info->cfg);

    if (source_info->phis == NULL)
        source_info->phis = computeRequiredPhis(compiled_func->clfunc->param_names, source_info->cfg,
                                                source_info->liveness, scope_info);

    std::vector<std::string> dead_symbols;
    for (auto&& it : sym_table) {
        if (!source_info->liveness->isLiveAtEnd(it.getKey(), current_block)) {
            dead_symbols.push_back(it.getKey());
        } else if (source_info->phis->isRequiredAfter(it.getKey(), current_block)) {
            assert(!scope_info->refersToGlobal(it.getKey()));
        } else {
        }
    }
    for (auto&& dead : dead_symbols)
        sym_table.erase(dead);
}

Value ASTInterpreter::doBinOp(Box* left, Box* right, int op, BinExpType exp_type) {
    if (op == AST_TYPE::Div && (source_info->parent_module->future_flags & FF_DIVISION)) {
        op = AST_TYPE::TrueDiv;
    }
    switch (exp_type) {
        case BinExpType::AugBinOp:
            return augbinop(left, right, op);
        case BinExpType::BinOp:
            return binop(left, right, op);
        case BinExpType::Compare:
            return compare(left, right, op);
        default:
            RELEASE_ASSERT(0, "not implemented");
    }
    return Value();
}

void ASTInterpreter::doStore(const std::string& name, Value value) {
    if (scope_info->refersToGlobal(name)) {
        setattr(source_info->parent_module, name.c_str(), value.o);
    } else {
        sym_table[name] = value.o;
        if (scope_info->saveInClosure(name))
            setattr(created_closure, name.c_str(), value.o);
    }
}

void ASTInterpreter::doStore(AST_expr* node, Value value) {
    if (node->type == AST_TYPE::Name) {
        AST_Name* name = (AST_Name*)node;
        doStore(name->id, value);
    } else if (node->type == AST_TYPE::Attribute) {
        AST_Attribute* attr = (AST_Attribute*)node;
        setattr(visit_expr(attr->value).o, attr->attr.c_str(), value.o);
    } else if (node->type == AST_TYPE::Tuple) {
        AST_Tuple* tuple = (AST_Tuple*)node;
        Box** array = unpackIntoArray(value.o, tuple->elts.size());
        unsigned i = 0;
        for (AST_expr* e : tuple->elts)
            doStore(e, array[i++]);
    } else if (node->type == AST_TYPE::List) {
        AST_List* list = (AST_List*)node;
        Box** array = unpackIntoArray(value.o, list->elts.size());
        unsigned i = 0;
        for (AST_expr* e : list->elts)
            doStore(e, array[i++]);
    } else if (node->type == AST_TYPE::Subscript) {
        AST_Subscript* subscript = (AST_Subscript*)node;

        Value target = visit_expr(subscript->value);
        Value slice = visit_expr(subscript->slice);

        setitem(target.o, slice.o, value.o);
    } else {
        RELEASE_ASSERT(0, "not implemented");
    }
}

Value ASTInterpreter::visit_unaryop(AST_UnaryOp* node) {
    Value operand = visit_expr(node->operand);
    if (node->op_type == AST_TYPE::Not)
        return boxBool(!nonzero(operand.o));
    else
        return unaryop(operand.o, node->op_type);
}

Value ASTInterpreter::visit_binop(AST_BinOp* node) {
    Value left = visit_expr(node->left);
    Value right = visit_expr(node->right);
    return doBinOp(left.o, right.o, node->op_type, BinExpType::BinOp);
}

Value ASTInterpreter::visit_slice(AST_Slice* node) {
    Value lower = node->lower ? visit_expr(node->lower) : None;
    Value upper = node->upper ? visit_expr(node->upper) : None;
    Value step = node->step ? visit_expr(node->step) : None;
    return createSlice(lower.o, upper.o, step.o);
}

Value ASTInterpreter::visit_branch(AST_Branch* node) {
    Value v = visit_expr(node->test);
    ASSERT(v.o == True || v.o == False, "Should have called NONZERO before this branch");

    if (v.o == True)
        next_block = node->iftrue;
    else
        next_block = node->iffalse;
    return Value();
}

Value ASTInterpreter::visit_jump(AST_Jump* node) {
    bool backedge = node->target->idx < current_block->idx && compiled_func;
    if (backedge)
        threading::allowGLReadPreemption();

    if (ENABLE_OSR && backedge) {
        ++edgecount;
        if (edgecount > OSR_THRESHOLD) {
            eraseDeadSymbols();

            const OSREntryDescriptor* found_entry = nullptr;
            for (auto& p : compiled_func->clfunc->osr_versions) {
                if (p.first->cf != compiled_func)
                    continue;
                if (p.first->backedge != node)
                    continue;

                found_entry = p.first;
            }

            std::map<std::string, Box*> sorted_symbol_table;

            auto phis = compiled_func->clfunc->source->phis;
            for (auto& name : phis->definedness.getDefinedNamesAtEnd(current_block)) {
                auto it = sym_table.find(name);
                if (!compiled_func->clfunc->source->liveness->isLiveAtEnd(name, current_block))
                    continue;

                if (phis->isPotentiallyUndefinedAfter(name, current_block)) {
                    bool is_defined = it != sym_table.end();
                    sorted_symbol_table[getIsDefinedName(name)] = (Box*)is_defined;
                    if (is_defined)
                        assert(it->getValue() != NULL);
                    sorted_symbol_table[name] = is_defined ? it->getValue() : NULL;
                } else {
                    ASSERT(it != sym_table.end(), "%s", name.c_str());
                    sorted_symbol_table[it->getKey()] = it->getValue();
                }
            }

            if (generator)
                sorted_symbol_table[PASSED_GENERATOR_NAME] = generator;

            if (passed_closure)
                sorted_symbol_table[PASSED_CLOSURE_NAME] = passed_closure;

            if (created_closure)
                sorted_symbol_table[CREATED_CLOSURE_NAME] = created_closure;

            if (found_entry == nullptr) {
                OSREntryDescriptor* entry = OSREntryDescriptor::create(compiled_func, node);

                for (auto& it : sorted_symbol_table) {
                    if (isIsDefinedName(it.first))
                        entry->args[it.first] = BOOL;
                    else if (it.first == PASSED_GENERATOR_NAME)
                        entry->args[it.first] = GENERATOR;
                    else if (it.first == PASSED_CLOSURE_NAME || it.first == CREATED_CLOSURE_NAME)
                        entry->args[it.first] = CLOSURE;
                    else {
                        assert(it.first[0] != '!');
                        entry->args[it.first] = UNKNOWN;
                    }
                }

                found_entry = entry;
            }

            OSRExit exit(compiled_func, found_entry);

            std::vector<Box*> arg_array;
            for (auto& it : sorted_symbol_table) {
                arg_array.push_back(it.second);
            }

            CompiledFunction* partial_func = compilePartialFuncInternal(&exit);
            Box* arg1 = arg_array.size() >= 1 ? arg_array[0] : 0;
            Box* arg2 = arg_array.size() >= 2 ? arg_array[1] : 0;
            Box* arg3 = arg_array.size() >= 3 ? arg_array[2] : 0;
            Box** args = arg_array.size() >= 4 ? &arg_array[3] : 0;
            return partial_func->call(arg1, arg2, arg3, args);
        }
    }

    next_block = node->target;
    return Value();
}

Value ASTInterpreter::visit_invoke(AST_Invoke* node) {
    Value v;
    try {
        v = visit_stmt(node->stmt);
        next_block = node->normal_dest;
    } catch (ExcInfo e) {
        next_block = node->exc_dest;
        last_exception = e;
    }

    return v;
}

Value ASTInterpreter::visit_clsAttribute(AST_ClsAttribute* node) {
    return getattr(visit_expr(node->value).o, node->attr.c_str());
}

Value ASTInterpreter::visit_augBinOp(AST_AugBinOp* node) {
    assert(node->op_type != AST_TYPE::Is && node->op_type != AST_TYPE::IsNot && "not tested yet");

    Value left = visit_expr(node->left);
    Value right = visit_expr(node->right);
    return doBinOp(left.o, right.o, node->op_type, BinExpType::AugBinOp);
}

Value ASTInterpreter::visit_langPrimitive(AST_LangPrimitive* node) {
    Value v;
    if (node->opcode == AST_LangPrimitive::GET_ITER) {
        assert(node->args.size() == 1);
        v = getPystonIter(visit_expr(node->args[0]).o);
    } else if (node->opcode == AST_LangPrimitive::IMPORT_FROM) {
        assert(node->args.size() == 2);
        assert(node->args[0]->type == AST_TYPE::Name);
        assert(node->args[1]->type == AST_TYPE::Str);

        Value module = visit_expr(node->args[0]);
        const std::string& name = ast_cast<AST_Str>(node->args[1])->s;
        assert(name.size());
        v = importFrom(module.o, &name);
    } else if (node->opcode == AST_LangPrimitive::IMPORT_NAME) {
        assert(node->args.size() == 3);
        assert(node->args[0]->type == AST_TYPE::Num);
        assert(static_cast<AST_Num*>(node->args[0])->num_type == AST_Num::INT);
        assert(node->args[2]->type == AST_TYPE::Str);

        int level = static_cast<AST_Num*>(node->args[0])->n_int;
        Value froms = visit_expr(node->args[1]);
        const std::string& module_name = static_cast<AST_Str*>(node->args[2])->s;
        v = import(level, froms.o, &module_name);
    } else if (node->opcode == AST_LangPrimitive::IMPORT_STAR) {
        assert(node->args.size() == 1);
        assert(node->args[0]->type == AST_TYPE::Name);

        RELEASE_ASSERT(source_info->ast->type == AST_TYPE::Module, "import * not supported in functions");

        Value module = visit_expr(node->args[0]);
        v = importStar(module.o, source_info->parent_module);
    } else if (node->opcode == AST_LangPrimitive::NONE) {
        v = None;
    } else if (node->opcode == AST_LangPrimitive::LANDINGPAD) {
        assert(last_exception.type);
        Box* type = last_exception.type;
        Box* value = last_exception.value ? last_exception.value : None;
        Box* traceback = last_exception.traceback ? last_exception.traceback : None;
        v = new BoxedTuple({ type, value, traceback });
        last_exception = ExcInfo(NULL, NULL, NULL);
    } else if (node->opcode == AST_LangPrimitive::ISINSTANCE) {
        assert(node->args.size() == 3);
        Value obj = visit_expr(node->args[0]);
        Value cls = visit_expr(node->args[1]);
        Value flags = visit_expr(node->args[2]);

        v = boxBool(isinstance(obj.o, cls.o, unboxInt(flags.o)));

    } else if (node->opcode == AST_LangPrimitive::LOCALS) {
        assert(node->args.size() == 0);
        BoxedDict* dict = new BoxedDict;
        for (auto& p : sym_table) {
            llvm::StringRef s = p.first();
            if (s[0] == '!' || s[0] == '#')
                continue;

            dict->d[new BoxedString(s.str())] = p.second;
        }
        v = dict;
    } else if (node->opcode == AST_LangPrimitive::NONZERO) {
        assert(node->args.size() == 1);
        Value obj = visit_expr(node->args[0]);
        v = boxBool(nonzero(obj.o));
    } else if (node->opcode == AST_LangPrimitive::SET_EXC_INFO) {
        assert(node->args.size() == 3);

        Value type = visit_expr(node->args[0]);
        assert(type.o);
        Value value = visit_expr(node->args[1]);
        assert(value.o);
        Value traceback = visit_expr(node->args[2]);
        assert(traceback.o);

        getFrameInfo()->exc = ExcInfo(type.o, value.o, traceback.o);
        v = None;
    } else
        RELEASE_ASSERT(0, "not implemented");
    return v;
}

Value ASTInterpreter::visit_yield(AST_Yield* node) {
    Value value = node->value ? visit_expr(node->value) : None;
    assert(generator && generator->cls == generator_cls);
    return yield(generator, value.o);
}

Value __attribute__((flatten)) ASTInterpreter::visit_stmt(AST_stmt* node) {
    if (0) {
        printf("%20s % 2d ", source_info->getName().c_str(), current_block->idx);
        print_ast(node);
        printf("\n");
    }

    switch (node->type) {
        case AST_TYPE::Assert:
            return visit_assert((AST_Assert*)node);
        case AST_TYPE::Assign:
            return visit_assign((AST_Assign*)node);
        case AST_TYPE::ClassDef:
            return visit_classDef((AST_ClassDef*)node);
        case AST_TYPE::Delete:
            return visit_delete((AST_Delete*)node);
        case AST_TYPE::Expr:
            return visit_expr((AST_Expr*)node);
        case AST_TYPE::FunctionDef:
            return visit_functionDef((AST_FunctionDef*)node);
        case AST_TYPE::Pass:
            return Value(); // nothing todo
        case AST_TYPE::Print:
            return visit_print((AST_Print*)node);
        case AST_TYPE::Raise:
            return visit_raise((AST_Raise*)node);
        case AST_TYPE::Return:
            return visit_return((AST_Return*)node);
        case AST_TYPE::Global:
            return visit_global((AST_Global*)node);

        // pseudo
        case AST_TYPE::Branch:
            return visit_branch((AST_Branch*)node);
        case AST_TYPE::Jump:
            return visit_jump((AST_Jump*)node);
        case AST_TYPE::Invoke:
            return visit_invoke((AST_Invoke*)node);
        default:
            RELEASE_ASSERT(0, "not implemented");
    };
    return Value();
}

Value ASTInterpreter::visit_return(AST_Return* node) {
    Value s(node->value ? visit_expr(node->value) : None);
    next_block = 0;
    return s;
}

Box* ASTInterpreter::createFunction(AST* node, AST_arguments* args, const std::vector<AST_stmt*>& body) {
    CLFunction* cl = wrapFunction(node, args, body, source_info);

    std::vector<Box*> defaults;
    for (AST_expr* d : args->defaults)
        defaults.push_back(visit_expr(d).o);
    defaults.push_back(0);

    // FIXME: Using initializer_list is pretty annoying since you're not supposed to create them:
    union {
        struct {
            Box** ptr;
            size_t s;
        } d;
        std::initializer_list<Box*> il = {};
    } u;

    u.d.ptr = &defaults[0];
    u.d.s = defaults.size() - 1;

    bool takes_closure;
    // Optimization: when compiling a module, it's nice to not have to run analyses into the
    // entire module's source code.
    // If we call getScopeInfoForNode, that will trigger an analysis of that function tree,
    // but we're only using it here to figure out if that function takes a closure.
    // Top level functions never take a closure, so we can skip the analysis.
    if (source_info->ast->type == AST_TYPE::Module)
        takes_closure = false;
    else {
        takes_closure = source_info->scoping->getScopeInfoForNode(node)->takesClosure();
    }

    bool is_generator = cl->source->is_generator;

    BoxedClosure* closure = 0;
    if (takes_closure) {
        if (scope_info->createsClosure()) {
            closure = created_closure;
        } else {
            assert(scope_info->passesThroughClosure());
            closure = passed_closure;
        }
        assert(closure);
    }

    return boxCLFunction(cl, closure, is_generator, u.il);
}

Value ASTInterpreter::visit_functionDef(AST_FunctionDef* node) {
    AST_arguments* args = node->args;

    std::vector<Box*> decorators;
    for (AST_expr* d : node->decorator_list)
        decorators.push_back(visit_expr(d).o);

    Box* func = createFunction(node, args, node->body);

    for (int i = decorators.size() - 1; i >= 0; i--)
        func = runtimeCall(decorators[i], ArgPassSpec(1), func, 0, 0, 0, 0);

    doStore(node->name, func);
    return Value();
}

Value ASTInterpreter::visit_classDef(AST_ClassDef* node) {
    ScopeInfo* scope_info = source_info->scoping->getScopeInfoForNode(node);
    assert(scope_info);

    BoxedTuple::GCVector bases;
    for (AST_expr* b : node->bases)
        bases.push_back(visit_expr(b).o);

    BoxedTuple* basesTuple = new BoxedTuple(std::move(bases));

    std::vector<Box*> decorators;
    for (AST_expr* d : node->decorator_list)
        decorators.push_back(visit_expr(d).o);

    BoxedClosure* closure = scope_info->takesClosure() ? created_closure : 0;
    CLFunction* cl = wrapFunction(node, nullptr, node->body, source_info);
    Box* attrDict = runtimeCall(boxCLFunction(cl, closure, false, {}), ArgPassSpec(0), 0, 0, 0, 0, 0);

    Box* classobj = createUserClass(&node->name, basesTuple, attrDict);

    for (int i = decorators.size() - 1; i >= 0; i--)
        classobj = runtimeCall(decorators[i], ArgPassSpec(1), classobj, 0, 0, 0, 0);

    doStore(node->name, classobj);
    return Value();
}

Value ASTInterpreter::visit_raise(AST_Raise* node) {
    if (node->arg0 == NULL) {
        assert(!node->arg1);
        assert(!node->arg2);
        raise0();
    }

    raise3(node->arg0 ? visit_expr(node->arg0).o : None, node->arg1 ? visit_expr(node->arg1).o : None,
           node->arg2 ? visit_expr(node->arg2).o : None);
    return Value();
}

Value ASTInterpreter::visit_assert(AST_Assert* node) {
#ifndef NDEBUG
    // Currently we only generate "assert 0" statements
    Value v = visit_expr(node->test);
    assert(v.o->cls == int_cls && static_cast<BoxedInt*>(v.o)->n == 0);
#endif
    assertFail(source_info->parent_module, node->msg ? visit_expr(node->msg).o : 0);

    return Value();
}

Value ASTInterpreter::visit_global(AST_Global* node) {
    for (std::string& name : node->names)
        sym_table.erase(name);
    return Value();
}

Value ASTInterpreter::visit_delete(AST_Delete* node) {
    for (AST_expr* target_ : node->targets) {
        switch (target_->type) {
            case AST_TYPE::Subscript: {
                AST_Subscript* sub = (AST_Subscript*)target_;
                Value value = visit_expr(sub->value);
                Value slice = visit_expr(sub->slice);
                delitem(value.o, slice.o);
                break;
            }
            case AST_TYPE::Attribute: {
                AST_Attribute* attr = (AST_Attribute*)target_;
                delattr(visit_expr(attr->value).o, attr->attr.c_str());
                break;
            }
            case AST_TYPE::Name: {
                AST_Name* target = (AST_Name*)target_;
                if (scope_info->refersToGlobal(target->id)) {
                    // Can't use delattr since the errors are different:
                    delGlobal(source_info->parent_module, &target->id);
                    continue;
                }

                assert(!scope_info->refersToClosure(target->id));
                assert(!scope_info->saveInClosure(
                    target->id)); // SyntaxError: can not delete variable 'x' referenced in nested scope

                // A del of a missing name generates different error messages in a function scope vs a classdef scope
                bool local_error_msg = (source_info->ast->type != AST_TYPE::ClassDef);

                if (sym_table.count(target->id) == 0) {
                    assertNameDefined(0, target->id.c_str(), NameError, local_error_msg);
                    return Value();
                }

                sym_table.erase(target->id);
                break;
            }
            default:
                ASSERT(0, "Unsupported del target: %d", target_->type);
                abort();
        }
    }
    return Value();
}

Value ASTInterpreter::visit_assign(AST_Assign* node) {
    Value v = visit_expr(node->value);
    for (AST_expr* e : node->targets)
        doStore(e, v);
    return Value();
}

Value ASTInterpreter::visit_print(AST_Print* node) {
    static const std::string write_str("write");
    static const std::string newline_str("\n");
    static const std::string space_str(" ");

    Box* dest = node->dest ? visit_expr(node->dest).o : getSysStdout();
    int nvals = node->values.size();
    for (int i = 0; i < nvals; i++) {
        Box* var = visit_expr(node->values[i]).o;

        // begin code for handling of softspace
        bool new_softspace = (i < nvals - 1) || (!node->nl);
        if (softspace(dest, new_softspace)) {
            callattrInternal(dest, &write_str, CLASS_OR_INST, 0, ArgPassSpec(1), boxString(space_str), 0, 0, 0, 0);
        }
        callattrInternal(dest, &write_str, CLASS_OR_INST, 0, ArgPassSpec(1), str(var), 0, 0, 0, 0);
    }

    if (node->nl) {
        callattrInternal(dest, &write_str, CLASS_OR_INST, 0, ArgPassSpec(1), boxString(newline_str), 0, 0, 0, 0);
        if (nvals == 0) {
            softspace(dest, false);
        }
    }
    return Value();
}

Value ASTInterpreter::visit_compare(AST_Compare* node) {
    RELEASE_ASSERT(node->comparators.size() == 1, "not implemented");
    return doBinOp(visit_expr(node->left).o, visit_expr(node->comparators[0]).o, node->ops[0], BinExpType::Compare);
}

Value __attribute__((flatten)) ASTInterpreter::visit_expr(AST_expr* node) {
    switch (node->type) {
        case AST_TYPE::Attribute:
            return visit_attribute((AST_Attribute*)node);
        case AST_TYPE::BinOp:
            return visit_binop((AST_BinOp*)node);
        case AST_TYPE::Call:
            return visit_call((AST_Call*)node);
        case AST_TYPE::Compare:
            return visit_compare((AST_Compare*)node);
        case AST_TYPE::Dict:
            return visit_dict((AST_Dict*)node);
        case AST_TYPE::Index:
            return visit_index((AST_Index*)node);
        case AST_TYPE::Lambda:
            return visit_lambda((AST_Lambda*)node);
        case AST_TYPE::List:
            return visit_list((AST_List*)node);
        case AST_TYPE::Name:
            return visit_name((AST_Name*)node);
        case AST_TYPE::Num:
            return visit_num((AST_Num*)node);
        case AST_TYPE::Repr:
            return visit_repr((AST_Repr*)node);
        case AST_TYPE::Set:
            return visit_set((AST_Set*)node);
        case AST_TYPE::Slice:
            return visit_slice((AST_Slice*)node);
        case AST_TYPE::Str:
            return visit_str((AST_Str*)node);
        case AST_TYPE::Subscript:
            return visit_subscript((AST_Subscript*)node);
        case AST_TYPE::Tuple:
            return visit_tuple((AST_Tuple*)node);
        case AST_TYPE::UnaryOp:
            return visit_unaryop((AST_UnaryOp*)node);
        case AST_TYPE::Yield:
            return visit_yield((AST_Yield*)node);

        // pseudo
        case AST_TYPE::AugBinOp:
            return visit_augBinOp((AST_AugBinOp*)node);
        case AST_TYPE::ClsAttribute:
            return visit_clsAttribute((AST_ClsAttribute*)node);
        case AST_TYPE::LangPrimitive:
            return visit_langPrimitive((AST_LangPrimitive*)node);
        default:
            RELEASE_ASSERT(0, "");
    };
    return Value();
}


Value ASTInterpreter::visit_call(AST_Call* node) {
    Value v;
    Value func;

    std::string* attr = nullptr;

    bool is_callattr = false;
    bool callattr_clsonly = false;
    if (node->func->type == AST_TYPE::Attribute) {
        is_callattr = true;
        callattr_clsonly = false;
        AST_Attribute* attr_ast = ast_cast<AST_Attribute>(node->func);
        func = visit_expr(attr_ast->value);
        attr = &attr_ast->attr;
    } else if (node->func->type == AST_TYPE::ClsAttribute) {
        is_callattr = true;
        callattr_clsonly = true;
        AST_ClsAttribute* attr_ast = ast_cast<AST_ClsAttribute>(node->func);
        func = visit_expr(attr_ast->value);
        attr = &attr_ast->attr;
    } else
        func = visit_expr(node->func);

    std::vector<Box*> args;
    for (AST_expr* e : node->args)
        args.push_back(visit_expr(e).o);

    std::vector<const std::string*> keywords;
    for (AST_keyword* k : node->keywords) {
        keywords.push_back(&k->arg);
        args.push_back(visit_expr(k->value).o);
    }

    if (node->starargs)
        args.push_back(visit_expr(node->starargs).o);

    if (node->kwargs)
        args.push_back(visit_expr(node->kwargs).o);

    ArgPassSpec argspec(node->args.size(), node->keywords.size(), node->starargs, node->kwargs);

    if (is_callattr) {
        return callattr(func.o, attr, CallattrFlags({.cls_only = callattr_clsonly, .null_on_nonexistent = false }),
                        argspec, args.size() > 0 ? args[0] : 0, args.size() > 1 ? args[1] : 0,
                        args.size() > 2 ? args[2] : 0, args.size() > 3 ? &args[3] : 0, &keywords);
    } else {
        return runtimeCall(func.o, argspec, args.size() > 0 ? args[0] : 0, args.size() > 1 ? args[1] : 0,
                           args.size() > 2 ? args[2] : 0, args.size() > 3 ? &args[3] : 0, &keywords);
    }
}


Value ASTInterpreter::visit_expr(AST_Expr* node) {
    return visit_expr(node->value);
}

Value ASTInterpreter::visit_num(AST_Num* node) {
    if (node->num_type == AST_Num::INT)
        return boxInt(node->n_int);
    else if (node->num_type == AST_Num::FLOAT)
        return boxFloat(node->n_float);
    else if (node->num_type == AST_Num::LONG)
        return createLong(&node->n_long);
    else if (node->num_type == AST_Num::COMPLEX)
        return boxComplex(0.0, node->n_float);
    RELEASE_ASSERT(0, "not implemented");
    return Value();
}

Value ASTInterpreter::visit_index(AST_Index* node) {
    return visit_expr(node->value);
}

Value ASTInterpreter::visit_repr(AST_Repr* node) {
    return repr(visit_expr(node->value).o);
}

Value ASTInterpreter::visit_lambda(AST_Lambda* node) {
    AST_Return* expr = new AST_Return();
    expr->value = node->body;

    std::vector<AST_stmt*> body = { expr };
    return createFunction(node, node->args, body);
}

Value ASTInterpreter::visit_dict(AST_Dict* node) {
    RELEASE_ASSERT(node->keys.size() == node->values.size(), "not implemented");
    BoxedDict* dict = new BoxedDict();
    for (size_t i = 0; i < node->keys.size(); ++i) {
        Box* v = visit_expr(node->values[i]).o;
        Box* k = visit_expr(node->keys[i]).o;
        dict->d[k] = v;
    }

    return dict;
}

Value ASTInterpreter::visit_set(AST_Set* node) {
    BoxedSet::Set set;
    for (AST_expr* e : node->elts)
        set.insert(visit_expr(e).o);

    return new BoxedSet(std::move(set));
}

Value ASTInterpreter::visit_str(AST_Str* node) {
    return boxString(node->s);
}

Value ASTInterpreter::visit_name(AST_Name* node) {
    if (scope_info->refersToGlobal(node->id))
        return getGlobal(source_info->parent_module, &node->id);
    else if (scope_info->refersToClosure(node->id)) {
        return getattr(passed_closure, node->id.c_str());
    } else {
        SymMap::iterator it = sym_table.find(node->id);
        if (it != sym_table.end()) {
            Box* value = it->second;
            return value;
        }

        // classdefs have different scoping rules than functions:
        if (source_info->ast->type == AST_TYPE::ClassDef)
            return getGlobal(source_info->parent_module, &node->id);

        assertNameDefined(0, node->id.c_str(), UnboundLocalError, true);
        return Value();
    }
}


Value ASTInterpreter::visit_subscript(AST_Subscript* node) {
    Value value = visit_expr(node->value);
    Value slice = visit_expr(node->slice);
    return getitem(value.o, slice.o);
}

Value ASTInterpreter::visit_list(AST_List* node) {
    BoxedList* list = new BoxedList;
    list->ensure(node->elts.size());
    for (AST_expr* e : node->elts)
        listAppendInternal(list, visit_expr(e).o);
    return list;
}

Value ASTInterpreter::visit_tuple(AST_Tuple* node) {
    BoxedTuple::GCVector elts;
    for (AST_expr* e : node->elts)
        elts.push_back(visit_expr(e).o);
    return new BoxedTuple(std::move(elts));
}

Value ASTInterpreter::visit_attribute(AST_Attribute* node) {
    return getattr(visit_expr(node->value).o, node->attr.c_str());
}
}
