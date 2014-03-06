#include <set>

#include "IR.h"
#include "Function.h"
#include "Scope.h"
#include "CSE.h"
#include "Random.h"

namespace Halide {
namespace Internal {

static void assertf(const Function &func, bool cond, const char* msg) {
    if (!cond) {
        if (func.debuginfo().empty()) {
            std::cerr << msg << " (Func: " << func.name() << ")\n";
        } else {
            std::cerr << func.debuginfo() << ": " << msg << " (Func: " << func.name() << ")\n";
        }
        assert(false);
    }
}

using std::vector;
using std::string;
using std::set;

template<>
EXPORT RefCount &ref_count<FunctionContents>(const FunctionContents *f) {return f->ref_count;}

template<>
EXPORT void destroy<FunctionContents>(const FunctionContents *f) {delete f;}

// All variables present in any part of a function definition must
// either be pure args, elements of the reduction domain, parameters
// (i.e. attached to some Parameter object), or part of a let node
// internal to the expression
struct CheckVars : public IRGraphVisitor {
    vector<string> pure_args;
    ReductionDomain reduction_domain;
    Scope<int> defined_internally;
    const Function &func;

    CheckVars(const Function &f) : func(f) {}

    using IRVisitor::visit;

    void visit(const Let *let) {
        let->value.accept(this);
        defined_internally.push(let->name, 0);
        let->body.accept(this);
        defined_internally.pop(let->name);
    }

    void visit(const Call *op) {
        IRGraphVisitor::visit(op);
        if (op->name == func.name() && op->call_type == Call::Halide) {
            for (size_t i = 0; i < op->args.size(); i++) {
                const Variable *var = op->args[i].as<Variable>();
                if (!pure_args[i].empty()) {
                    assertf(func, var && var->name == pure_args[i],
                            "All of a functions recursive references to itself"
                            " must contain the same pure variables in the same"
                            " places as on the left-hand-side.");
                }
            }
        }
    }

    void visit(const Variable *var) {
        // Is it a parameter?
        if (var->param.defined()) return;

        // Was it defined internally by a let expression?
        if (defined_internally.contains(var->name)) return;

        // Is it a pure argument?
        for (size_t i = 0; i < pure_args.size(); i++) {
            if (var->name == pure_args[i]) return;
        }

        // Is it in a reduction domain?
        if (var->reduction_domain.defined()) {
            if (!reduction_domain.defined()) {
                reduction_domain = var->reduction_domain;
                return;
            } else if (var->reduction_domain.same_as(reduction_domain)) {
                // It's in a reduction domain we already know about
                return;
            } else {
                assertf(func, false, "Multiple reduction domains found in function definition");
            }
        }

        if (!func.debuginfo().empty())
            std::cerr << func.debuginfo() << ": ";
        std::cerr << "Undefined variable in function definition: " << var->name
                  << " (Func: " << func.name() << ")\n";
        assert(false);
    }
};

struct CountSelfReferences : public IRGraphVisitor {
    set<const Call *> calls;
    const Function *func;

    using IRVisitor::visit;

    void visit(const Call *c) {
        if (c->func.same_as(*func)) {
            calls.insert(c);
        }
    }
};

// A counter to use in tagging random variables
namespace {
static int rand_counter = 0;
}

void Function::define(const vector<string> &args, vector<Expr> values) {
    assertf(*this, !has_extern_definition(), "Function with extern definition cannot be given a pure definition");
    assertf(*this, !name().empty(), "A function needs a name");
    for (size_t i = 0; i < values.size(); i++) {
        assertf(*this, values[i].defined(), "Undefined expression in right-hand-side of function definition");
    }

    // Make sure all the vars in the value are either args or are
    // attached to some parameter
    CheckVars check(*this);
    check.pure_args = args;
    for (size_t i = 0; i < values.size(); i++) {
        values[i].accept(&check);
    }

    // Make sure all the vars in the args have unique non-empty names
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i].empty()) { 
            if (!debuginfo().empty())
                std::cerr << debuginfo() << ": ";
            std::cerr << "In the left-hand-side of the definition of " << name()
                      << ", argument " << i << " has an empty name\n";
            assert(false);
        }
        for (size_t j = 0; j < i; j++) {
            if (args[i] == args[j]) {
                if (!debuginfo().empty())
                    std::cerr << debuginfo() << ": ";
                std::cerr << "In the left-hand-side of the definition of " << name()
                          << ", arguments " << j << " and " << i << " have the same name: " << args[i] << "\n";
                assert(false);
            }
        }
    }

    for (size_t i = 0; i < values.size(); i++) {
        values[i] = common_subexpression_elimination(values[i]);
    }

    // Tag calls to random() with the free vars
    int tag = rand_counter++;    
    for (size_t i = 0; i < values.size(); i++) {
        values[i] = lower_random(values[i], args, tag);
    }

    assertf(*this, !check.reduction_domain.defined(), "Reduction domain referenced in pure function definition");

    if (!contents.defined()) {
        contents = new FunctionContents;
        contents.ptr->name = unique_name('f');
    }

    assertf(*this, contents.ptr->values.empty(), "Function is already defined");
    contents.ptr->values = values;
    contents.ptr->args = args;

    contents.ptr->output_types.resize(values.size());
    for (size_t i = 0; i < contents.ptr->output_types.size(); i++) {
        contents.ptr->output_types[i] = values[i].type();
    }

    for (size_t i = 0; i < args.size(); i++) {
        Schedule::Dim d = {args[i], For::Serial};
        contents.ptr->schedule.dims.push_back(d);
        contents.ptr->schedule.storage_dims.push_back(args[i]);
    }

    for (size_t i = 0; i < values.size(); i++) {
        string buffer_name = name();
        if (values.size() > 1) {
            buffer_name += '.' + int_to_string((int)i);
        }
        contents.ptr->output_buffers.push_back(Parameter(values[i].type(), true, buffer_name));
    }
}

void Function::define_reduction(const vector<Expr> &_args, vector<Expr> values) {
    assertf(*this, !name().empty(), "A function needs a name");
    assertf(*this, has_pure_definition(), "Can't add a reduction definition without a regular definition first");

    for (size_t i = 0; i < values.size(); i++) {
        assertf(*this, values[i].defined(), "Undefined expression in right-hand-side of reduction");
    }

    // Check the dimensionality matches
    assertf(*this, (int)_args.size() == dimensions(),
           "Dimensionality of reduction definition must match dimensionality of pure definition");

    assertf(*this, values.size() == contents.ptr->values.size(),
            "Number of tuple elements for reduction definition must "
            "match number of tuple elements for pure definition");

    for (size_t i = 0; i < values.size(); i++) {
        // Check that pure value and the reduction value have the same
        // type.  Without this check, allocations may be the wrong size
        // relative to what update code expects.
        assertf(*this, contents.ptr->values[i].type() == values[i].type(),
                "Reduction definition does not match type of pure function definition.");
        values[i] = common_subexpression_elimination(values[i]);
    }

    vector<Expr> args(_args.size());
    for (size_t i = 0; i < args.size(); i++) {
        args[i] = common_subexpression_elimination(_args[i]);
    }

    // The pure args are those naked vars in the args that are not in
    // a reduction domain and are not parameters and line up with the
    // pure args in the pure definition.
    bool pure = true;
    vector<string> pure_args(args.size());
    for (size_t i = 0; i < args.size(); i++) {
        pure_args[i] = ""; // Will never match a var name
        assertf(*this, args[i].defined(), "Undefined expression in left-hand-side of reduction");
        if (const Variable *var = args[i].as<Variable>()) {
            if (!var->param.defined() &&
                !var->reduction_domain.defined() &&
                var->name == contents.ptr->args[i]) {
                pure_args[i] = var->name;
            } else {
                pure = false;
            }
        } else {
            pure = false;
        }
    }

    // Make sure all the vars in the args and the value are either
    // pure args, in the reduction domain, or a parameter. Also checks
    // that recursive references to the function contain all the pure
    // vars in the LHS in the correct places.
    CheckVars check(name());
    check.pure_args = pure_args;
    for (size_t i = 0; i < args.size(); i++) {
        args[i].accept(&check);
    }
    for (size_t i = 0; i < values.size(); i++) {
        values[i].accept(&check);
    }

    // Tag calls to random() with the free vars
    vector<string> free_vars;
    for (size_t i = 0; i < pure_args.size(); i++) {
        if (!pure_args[i].empty()) {
            free_vars.push_back(pure_args[i]);
        }
    }
    if (check.reduction_domain.defined()) {
        for (size_t i = 0; i < check.reduction_domain.domain().size(); i++) {
            string rvar = check.reduction_domain.domain()[i].var;
            free_vars.push_back(rvar);
        }
    }
    int tag = rand_counter++;    
    for (size_t i = 0; i < args.size(); i++) {
        args[i] = lower_random(args[i], free_vars, tag);
    }
    for (size_t i = 0; i < values.size(); i++) {
        values[i] = lower_random(values[i], free_vars, tag);
    }

    ReductionDefinition r;
    r.args = args;
    r.values = values;
    r.domain = check.reduction_domain;

    // The reduction value and args probably refer back to the
    // function itself, introducing circular references and hence
    // memory leaks. We need to count the number of unique call nodes
    // that point back to this function in order to break the cycles.
    CountSelfReferences counter;
    counter.func = this;
    for (size_t i = 0; i < args.size(); i++) {
        args[i].accept(&counter);
    }
    for (size_t i = 0; i < values.size(); i++) {
        values[i].accept(&counter);
    }

    for (size_t i = 0; i < counter.calls.size(); i++) {
        contents.ptr->ref_count.decrement();
        assertf(*this, !contents.ptr->ref_count.is_zero(),
                "Bug: removed too many circular references when defining reduction");
    }

    // First add any reduction domain
    if (r.domain.defined()) {
        for (size_t i = 0; i < r.domain.domain().size(); i++) {
            Schedule::Dim d = {r.domain.domain()[i].var, For::Serial};
            r.schedule.dims.push_back(d);
        }
    }

    // Then add the pure args outside of that
    for (size_t i = 0; i < pure_args.size(); i++) {
        if (!pure_args[i].empty()) {
            Schedule::Dim d = {pure_args[i], For::Serial};
            r.schedule.dims.push_back(d);
        }
    }

    // If there's no recursive reference, no reduction domain, and all
    // the args are pure, then this definition completely hides
    // earlier ones!
    if (!r.domain.defined() &&
        counter.calls.empty() &&
        pure) {
        std::cerr << "Warning: update definition " << contents.ptr->reductions.size()
                  << " of function " << name() << " completely hides earlier definitions, "
                  << " because all the arguments are pure, it contains no self-references, "
                  << " and no reduction domain. This may be an accidental re-definition of "
                  << " an already-defined function.\n";
    }

    contents.ptr->reductions.push_back(r);

}

void Function::define_extern(const std::string &function_name,
                             const std::vector<ExternFuncArgument> &args,
                             const std::vector<Type> &types,
                             int dimensionality) {

    assertf(*this, !has_pure_definition() && !has_reduction_definition(),
            "Function with a pure definition cannot have an extern definition");

    assertf(*this, !has_extern_definition(),
            "Function already has an extern definition");

    contents.ptr->extern_function_name = function_name;
    contents.ptr->extern_arguments = args;
    contents.ptr->output_types = types;

    for (size_t i = 0; i < types.size(); i++) {
        string buffer_name = name();
        if (types.size() > 1) {
            buffer_name += '.' + int_to_string((int)i);
        }
        contents.ptr->output_buffers.push_back(Parameter(types[i], true, buffer_name));
    }

    // Make some synthetic var names for scheduling purposes (e.g. reorder_storage).
    contents.ptr->args.resize(dimensionality);
    for (int i = 0; i < dimensionality; i++) {
        string arg = unique_name('e');
        contents.ptr->args[i] = arg;
        contents.ptr->schedule.storage_dims.push_back(arg);
    }

}

}
}
