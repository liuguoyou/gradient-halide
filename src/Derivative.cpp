#include "Derivative.h"

#include "BoundaryConditions.h"
#include "Simplify.h"
#include "Solve.h"
#include "Substitute.h"
#include "IRMutator.h"
#include "IROperator.h"
#include "IREquality.h"
#include "Error.h"
#include "runtime/printer.h"

#include <iostream>
#include <cmath>

namespace Halide {
namespace Internal {

class VariableFinder : public IRGraphVisitor {
public:
    bool find(const Expr &expr, const Var &var) {
        visited.clear();
        var_name = var.name();
        found = false;
        expr.accept(this);
        return found;
    }

    void visit(const Variable *op) {
        if (op->name == var_name) {
            found = true;
        }
    }

private:
    std::string var_name;
    bool found;
};

std::pair<Expr, Expr> get_min_max_bounds(const Expr &expr, const std::vector<Var> &current_args,
                                         const RDom &current_bounds, const int index) {
    if (expr.get()->node_type == IRNodeType::Add) {
        const Add *op = expr.as<Add>();
        const std::pair<Expr, Expr> a_bounds = get_min_max_bounds(op->a, current_args, current_bounds, index);
        const std::pair<Expr, Expr> b_bounds = get_min_max_bounds(op->b, current_args, current_bounds, index);
        debug(0) << "  " << index << " bounds for Add\n";
        return {a_bounds.first + b_bounds.first, a_bounds.second + b_bounds.second};
    } else if (expr.get()->node_type == IRNodeType::Sub) {
        const Sub *op = expr.as<Sub>();
        const std::pair<Expr, Expr> a_bounds = get_min_max_bounds(op->a, current_args, current_bounds, index);
        const std::pair<Expr, Expr> b_bounds = get_min_max_bounds(op->b, current_args, current_bounds, index);
        debug(0) << "  " << index << " bounds for Sub\n";
        return {a_bounds.first - b_bounds.second, a_bounds.second - b_bounds.first};
    } else if (expr.get()->node_type == IRNodeType::Variable) {
        const Variable *var = expr.as<Variable>();
        if (var->reduction_domain.defined()) {
            ReductionVariable rvar = var->reduction_domain.domain()[index];
            debug(0) << "  " << index << " bounds for Rvar\n";
            return {rvar.min, rvar.min + rvar.extent - 1};
        } else {
            debug(0) << "  " << index << " bounds for Var\n";
            for (int i = 0; i < (int)current_args.size(); i++) {
                if (current_args[i].name() == var->name) {
                    return {current_bounds[i].min(), current_bounds[i].extent()};
                }
            }
        }
    } else if (expr.get()->node_type == IRNodeType::Max) {
        const Max *op = expr.as<Max>();
        const std::pair<Expr, Expr> a_bounds = get_min_max_bounds(op->a, current_args, current_bounds, index);
        const std::pair<Expr, Expr> b_bounds = get_min_max_bounds(op->b, current_args, current_bounds, index);
        debug(0) << "  " << index << " bounds for Max\n";
        return {max(a_bounds.first, b_bounds.first), max(a_bounds.second, b_bounds.second)};
    } else if (expr.get()->node_type == IRNodeType::Min) {
        const Min *op = expr.as<Min>();
        const std::pair<Expr, Expr> a_bounds = get_min_max_bounds(op->a, current_args, current_bounds, index);
        const std::pair<Expr, Expr> b_bounds = get_min_max_bounds(op->b, current_args, current_bounds, index);
        debug(0) << "  " << index << " bounds for Min\n";
        return {min(a_bounds.first, b_bounds.first), min(a_bounds.second, b_bounds.second)};
    } else if (expr.get()->node_type == IRNodeType::IntImm) {
        debug(0) << "  " << index << " bounds for IntImm\n";
        return {expr, expr};
    }

    internal_error << "Can't infer bounds, Expr type not handled\n";
    return std::pair<Expr, Expr>();
}

std::pair<Expr, Expr> merge_bounds(const std::pair<Expr, Expr> &bounds0, const std::pair<Expr, Expr> &bounds1) {
    return {simplify(min(bounds0.first, bounds1.first)), simplify(max(bounds0.second, bounds1.second))};
};

/** An IR graph visitor that gather the function DAG and sort them in reverse topological order
 */
class FunctionSorter : public IRGraphVisitor {
public:
    void sort(const Expr &expr);
    void sort(const Func &func);

    std::vector<Func> get_functions() const {
        return functions;
    }

    void visit(const Call *op);

private:
    std::vector<Func> functions;
    std::set<std::string> traversed_functions;
};

void FunctionSorter::sort(const Expr &expr) {
    visited.clear();
    expr.accept(this);
}

void FunctionSorter::sort(const Func &func) {
    traversed_functions.insert(func.name());
    functions.push_back(Func(func));
    // Traverse from the last update to first
    for (int update_id = func.num_update_definitions() - 1; update_id >= -1; update_id--) {
        if (update_id >= 0) {
            func.update_value(update_id).accept(this);
        } else {
            func.value().accept(this);
        }
    }
}

void FunctionSorter::visit(const Call *op) {
    if (op->call_type == Call::Halide) {
        Func func(Function(op->func));
        if (traversed_functions.find(func.name()) != traversed_functions.end()) {
            return;
        }
        sort(func);
        return;
    }

    for (size_t i = 0; i < op->args.size(); i++) {
        include(op->args[i]);
    }
}


/** An IR graph visitor that gather the expression DAG and sort them in topological order
 */
class ExpressionSorter : public IRGraphVisitor {
public:
    void sort(const Expr &expr);

    std::vector<Expr> get_expr_list() const {
        return expr_list;
    }

    void visit(const Call *op);
protected:
    void include(const Expr &e);
private:
    std::vector<Expr> expr_list;
};

void ExpressionSorter::sort(const Expr &e) {
    visited.clear();
    expr_list.clear();
    e.accept(this);
    expr_list.push_back(e);
}

void ExpressionSorter::visit(const Call *op) {
    // No point visiting the arguments of a Halide func or an image
    if (op->call_type == Call::Halide || op->call_type == Call::Image) {
        return;
    }

    for (size_t i = 0; i < op->args.size(); i++) {
        include(op->args[i]);
    }
}


void ExpressionSorter::include(const Expr &e) {
    if (visited.count(e.get())) {
        return;
    } else {
        visited.insert(e.get());
        e.accept(this);
        expr_list.push_back(e);
        return;
    }
}

using FuncBounds = std::vector<std::pair<Expr, Expr>>;
using FuncKey = std::pair<std::string, int>; // function name & update_id, for initialization update_id == -1

/**
 *  Visit function calls and determine their bounds.
 *  So when we do f(x, y) = ... we know what the loop bounds are
 */
class BoundsInferencer : public IRVisitor {
public:
    void inference(const Expr &expr);
    void inference(const Func &func);

    void visit(const Call *op);

    std::map<FuncKey, RDom> get_func_bounds() const {
        // TODO(mgharbi): don't recompute that all the time..
        std::map<FuncKey, RDom> ret;
        // Convert to an Rdom
        for(auto b: func_bounds) { 
          debug(0) << "Computed bounds for " << b.first.first << "[" << b.first.second << "]" << ":\n";
          FuncBounds min_extent_bounds;
          min_extent_bounds.reserve(b.second.size());
          for (int i = 0; i < (int)b.second.size(); ++i) {
            Expr lower_bound = simplify(b.second[i].first);
            Expr extent = simplify(b.second[i].second - lower_bound+1);
            min_extent_bounds.push_back(std::make_pair(lower_bound, extent));
            debug(0) << "  arg" << i << " ("  << lower_bound << ", " << extent << ")\n";
          }
          ret[b.first] = RDom(min_extent_bounds);
        }
        return ret;
    }

private:
    int recursion_depth;
    std::map<FuncKey, FuncBounds> func_bounds;
    FuncKey current_func_key;
    std::vector<Var> current_args;
    RDom current_bounds;
};

void BoundsInferencer::inference(const Expr &expr) {
    // initialization
    func_bounds.clear();
    recursion_depth = 0;
    current_func_key = FuncKey{"", -1};
    current_args.clear();
    current_bounds = RDom();

    expr.accept(this);
}

void BoundsInferencer::inference(const Func &func) {
    FuncKey previous_func_key = current_func_key;
    RDom previous_bounds = current_bounds;
    std::vector<Var> previous_args = current_args;

    // Traverse from the last update to first
    for (int update_id = func.num_update_definitions() - 1; update_id >= -1; update_id--) {
        current_func_key = FuncKey{func.name(), update_id};
        current_bounds = RDom(func_bounds[current_func_key]);
        current_args = func.args();
        if (update_id >= 0) {
            func.update_value(update_id).accept(this);
        } else {
            func.value().accept(this);
        }
    }

    current_func_key = previous_func_key;
    current_args = previous_args;
    current_bounds = previous_bounds;
}

void BoundsInferencer::visit(const Call *op) {
    if (op->call_type == Call::Halide) {
        Func func(Function(op->func));
        debug(0) << recursion_depth << " Visiting " << func.name() << "\n";

        FuncBounds arg_bounds;
        arg_bounds.reserve(op->args.size());
        for (int i = 0; i < (int)op->args.size(); i++) {
            std::pair<Expr, Expr> min_max_bounds = get_min_max_bounds(op->args[i], current_args, current_bounds, i);
            arg_bounds.push_back(min_max_bounds);
        }

        // Update function bounds
        FuncKey key = current_func_key.first == func.name() ?
            FuncKey{func.name(), current_func_key.second - 1} :
            FuncKey{func.name(), func.num_update_definitions() - 1};

        if (func_bounds.find(key) != func_bounds.end()) {
            FuncBounds prev_bounds = func_bounds[key];
            assert(arg_bounds.size() == prev_bounds.size());
            for (int i = 0; i < (int)arg_bounds.size(); i++) {
                arg_bounds[i] = merge_bounds(prev_bounds[i], arg_bounds[i]);
            }
            debug(0) << "  Updated function bounds:" << "\n";
        }

        for (int i = 0; i < (int)arg_bounds.size(); i++) {
          debug(0) << "    arg" << i << " (" 
                   << arg_bounds[i].first << ", "
                   << arg_bounds[i].second << ")\n";
        }

        func_bounds[key] = arg_bounds;

        // Don't recurse if the target is the same function
        if (current_func_key.first != func.name()) {
            recursion_depth += 1;
            inference(func);
            recursion_depth -= 1;
        }
        return;
    }

    for (size_t i = 0; i < op->args.size(); i++) {
        // include(op->args[i]);
        op->args[i].accept(this);
    }
}


/** An IR visitor that computes the derivatives through reverse accumulation
 */
class ReverseAccumulationVisitor : public IRVisitor {
public:
    void propagate_adjoints(const Expr &output, const std::vector<Func> &funcs);
    std::map<std::string, Func> get_adjoint_funcs() const {
        // TOOD: avoid recomputation
        std::map<std::string, Func> ret;
        for (const auto &it : adjoint_funcs) {
            if (it.first.second == -1) { // XXX: is this correct?
                ret.insert(std::make_pair(it.first.first, it.second));
            }
        }
        return ret;
    }

protected:
    void visit(const Cast *op);
    void visit(const Variable *op);
    void visit(const Add *op);
    void visit(const Sub *op);
    void visit(const Mul *op);
    void visit(const Div *op);
    void visit(const Min *op);
    void visit(const Max *op);
    void visit(const Call *op);
    void visit(const Let *op);

private:
    void accumulate(const Expr &stub, const Expr &adjoint);

    std::map<const BaseExprNode *, Expr> accumulated_adjoints;
    std::map<FuncKey, Func> adjoint_funcs;
    std::map<std::string, Expr> let_var_mapping; // TODO: replace this with Scope
    std::map<FuncKey, RDom> func_bounds;
    FuncKey current_func_key;
    RDom current_bounds;
};

std::vector<std::pair<Expr, Expr>> rdom_to_vector(const RDom &bounds) {
    std::vector<std::pair<Expr, Expr>> ret;
    ret.reserve(bounds.domain().domain().size());
    for (const auto &rvar : bounds.domain().domain()) {
        ret.push_back({rvar.min, rvar.extent});
    }
    return ret;
};

void ReverseAccumulationVisitor::propagate_adjoints(const Expr &output, const std::vector<Func> &funcs) {
    if (funcs.size() == 0) {
        debug(0) << "ReverseAccumulationVisitor: no functions to backpropagate to.\n";
        return;
    }

    BoundsInferencer bounds_inferencer;
    debug(0) << "ReverseAccumulationVisitor: infering bounds.\n";
    bounds_inferencer.inference(output);
    func_bounds = bounds_inferencer.get_func_bounds();

    // Create a stub for each function to accumulate adjoints
    // Meanwhile set up boundary condition
    for (int func_id = 0; func_id < (int)funcs.size(); func_id++) {
        const Func &func = funcs[func_id];
        for (int update_id = -1; update_id < func.num_update_definitions(); update_id++) {
            Func adjoint_func(func.name() + "_" + std::to_string(update_id + 1) + "_d__");
            adjoint_func(func.args()) = 0.f;
            adjoint_funcs[FuncKey{func.name(), update_id}] = adjoint_func;
        }
    }

    // Propagate output
    ExpressionSorter sorter;
    sorter.sort(output);
    std::vector<Expr> expr_list = sorter.get_expr_list();
    accumulate(output, 1.f);

    // Traverse the expressions in reverse order
    for (auto it = expr_list.rbegin(); it != expr_list.rend(); it++) {
        // Propagate adjoints
        it->accept(this);
    }

    // Traverse functions
    for (int func_id = 0; func_id < (int)funcs.size(); func_id++) {
        const Func &func = funcs[func_id];
        // Traverse from the last update to first
        for (int update_id = func.num_update_definitions() - 1; update_id >= -1; update_id--) {
            // Topologically sort the expressions
            ExpressionSorter sorter;
            if (update_id >= 0) {
                sorter.sort(func.update_value(update_id));
            } else {
                sorter.sort(func.value());
            }

            FuncKey func_key{func.name(), update_id};
            // TODO: take lhs other than (x, y, z) into account
            assert(func_bounds.find(func_key) != func_bounds.end());
            current_func_key = func_key;
            current_bounds = func_bounds[func_key];

            // Set up boundary condition
            adjoint_funcs[func_key] = BoundaryConditions::constant_exterior(
                adjoint_funcs[func_key], 0.f, rdom_to_vector(func_bounds[func_key]));

            std::vector<Expr> expr_list = sorter.get_expr_list();
            // Retrieve previously propagated adjoint
            std::vector<Expr> args;
            for (const auto &arg : func.args()) {
                args.push_back(arg);
            }
            accumulated_adjoints[(const BaseExprNode *)expr_list.back().get()] =
                Call::make(adjoint_funcs[func_key].function(), args);

            // Traverse the expressions in reverse order
            for (auto it = expr_list.rbegin(); it != expr_list.rend(); it++) {
                // Propagate adjoints
                it->accept(this);
            }
        }
    }
}

void ReverseAccumulationVisitor::accumulate(const Expr &stub, const Expr &adjoint) {
    const BaseExprNode *stub_ptr = (const BaseExprNode *)stub.get();
    if (accumulated_adjoints.find(stub_ptr) == accumulated_adjoints.end()) {
        accumulated_adjoints[stub_ptr] = adjoint;
    } else {
        accumulated_adjoints[stub_ptr] += adjoint;
    }
}

void ReverseAccumulationVisitor::visit(const Cast *op) {
    assert(accumulated_adjoints.find(op) != accumulated_adjoints.end());
    Expr adjoint = accumulated_adjoints[op];

    // d/dx cast(x) = 1
    accumulate(op->value, adjoint);
}

void ReverseAccumulationVisitor::visit(const Variable *op) {
    assert(accumulated_adjoints.find(op) != accumulated_adjoints.end());
    Expr adjoint = accumulated_adjoints[op];

    auto it = let_var_mapping.find(op->name);
    if (it != let_var_mapping.end()) {
        accumulate(it->second, Let::make(op->name, it->second, adjoint));
    }
}

void ReverseAccumulationVisitor::visit(const Add *op) {
    assert(accumulated_adjoints.find(op) != accumulated_adjoints.end());
    Expr adjoint = accumulated_adjoints[op];

    // d/da a + b = 1
    accumulate(op->a, adjoint);
    // d/db a + b = 1
    accumulate(op->b, adjoint);
}

void ReverseAccumulationVisitor::visit(const Sub *op) {
    assert(accumulated_adjoints.find(op) != accumulated_adjoints.end());
    Expr adjoint = accumulated_adjoints[op];

    // d/da a - b = 1
    accumulate(op->a, adjoint);
    // d/db a - b = -1
    accumulate(op->b, -adjoint);
}

void ReverseAccumulationVisitor::visit(const Mul *op) {
    assert(accumulated_adjoints.find(op) != accumulated_adjoints.end());
    Expr adjoint = accumulated_adjoints[op];

    // d/da a * b = b
    accumulate(op->a, adjoint * op->b);
    // d/db a * b = a
    accumulate(op->b, adjoint * op->a);
}

void ReverseAccumulationVisitor::visit(const Div *op) {
    assert(accumulated_adjoints.find(op) != accumulated_adjoints.end());
    Expr adjoint = accumulated_adjoints[op];

    // d/da a / b = 1 / b
    accumulate(op->a, adjoint / op->b);
    // d/db a / b = - a / b^2
    accumulate(op->b, - adjoint * op->a / (op->b * op->b));
}

void ReverseAccumulationVisitor::visit(const Min *op) {
    assert(accumulated_adjoints.find(op) != accumulated_adjoints.end());
    Expr adjoint = accumulated_adjoints[op];

    // d/da min(a, b) = a <= b ? 1 : 0
    accumulate(op->a, select(op->a <= op->b, adjoint, 0.f));
    // d/db min(a, b) = b <= a ? 1 : 0
    accumulate(op->b, select(op->b <= op->a, adjoint, 0.f));
}

void ReverseAccumulationVisitor::visit(const Max *op) {
    assert(accumulated_adjoints.find(op) != accumulated_adjoints.end());
    Expr adjoint = accumulated_adjoints[op];

    // d/da max(a, b) = a >= b ? 1 : 0
    accumulate(op->a, select(op->a >= op->b, adjoint, 0.f));
    // d/db max(a, b) = b >= a ? 1 : 0
    accumulate(op->b, select(op->b >= op->a, adjoint, 0.f));
}

void ReverseAccumulationVisitor::visit(const Call *op) {
    assert(accumulated_adjoints.find(op) != accumulated_adjoints.end());
    Expr adjoint = accumulated_adjoints[op];
    if (op->name == "exp_f32") {
        // d/dx exp(x) = exp(x)
        for (size_t i = 0; i < op->args.size(); i++) {
            accumulate(op->args[i], adjoint * exp(op->args[i]));
        }
    }

    if (op->func.defined()) {
        // This is a Halide function call
        Function func(op->func);
        // Gather the domain variables of the function
        std::vector<std::string> func_args = func.args();
        std::vector<Var> args;
        std::for_each(func_args.begin(), func_args.end(),
                      [&args](const std::string &name){ args.push_back(Var(name)); });
        // We are scattering to this function
        debug(0) << "Scattering to " << func.name() << "\n";
        debug(0) << "op->args:" << "\n";
        for (const auto &arg : op->args) {
            debug(0) << arg << "\n";
        }
        debug(0) << "adjoint is:" << adjoint << "\n";
        // If referring to the current function itself, send to previous update
        Func& func_to_update = func.name() != current_func_key.first ?
            adjoint_funcs[FuncKey{func.name(), func.updates().size() - 1}] :
            adjoint_funcs[FuncKey{func.name(), current_func_key.second - 1}];
        // We want to do this:
        // func_to_update(op->args) += adjoint;
        // But op->args can be invalid lhs, need to canonicalize

        VariableFinder finder;
        // We canonicalize the left hand side arguments (op->args) so that it's always x, y, z, ...
        for (int i = 0; i < (int)op->args.size(); i++) {
            if (!finder.find(op->args[i], args[i])) {
                // When an argument x doesn't appear in op->args,
                // all x in adjoint needs to be replaced by a RDom looping through the bounds
                // of the current function
                if (finder.find(adjoint, args[i])) {
                    adjoint = substitute(args[i].name(), current_bounds, adjoint);
                }
                // If it's a RVar, we need to replace it with the non-reduction argument
                if (op->args[i].get()->node_type == IRNodeType::Variable) {
                    const Variable *var = op->args[i].as<Variable>();
                    if (var->reduction_domain.defined()) {
                        adjoint = substitute(var->name, args[i], adjoint);
                    }
                }
            } else {
                // Apply the inverse to rhs
                Var tmp("tmp");
                SolverResult result = solve_expression(tmp == op->args[i], args[i].name());
                if (!result.fully_solved) {
                    internal_error << "Can't solve the inverse";
                }
                assert(result.result.as<EQ>() != nullptr);
                Expr result_rhs = result.result.as<EQ>()->b;
                Expr inv = substitute(tmp.name(), args[i], result_rhs);
                // tmp = f(x)
                // x = f^{-1}(tmp)
                // substitute(f^{-1}, tmp, x)
                adjoint = substitute(args[i].name(), inv, adjoint);
            }
        }

        debug(0) << "adjoint after canonicalization:" << adjoint << "\n";
        func_to_update(args) += adjoint;
        print_func(func_to_update);
    }
}

void ReverseAccumulationVisitor::visit(const Let *op) {
    assert(accumulated_adjoints.find(op) != accumulated_adjoints.end());
    Expr adjoint = accumulated_adjoints[op];

    accumulate(op->body, adjoint);
    let_var_mapping[op->name] = op->value;
}

} // namespace Internal


std::map<std::string, Func> propagate_adjoints(const Expr &output) {
    Internal::FunctionSorter sorter;
    Internal::debug(0) << "Propagate: Sorting functions" << "\n";
    sorter.sort(output);
    std::vector<Func> funcs = sorter.get_functions();
    Internal::debug(0) << "Propagate: Sorted Func list:" << "\n";
    for (const auto &func : funcs) {
        Internal::debug(0) << "  . " << func.name() << "\n";
    }
    Internal::ReverseAccumulationVisitor visitor;
    visitor.propagate_adjoints(output, funcs);
    return visitor.get_adjoint_funcs();
}

void print_func(const Func &func) {
    Internal::debug(0) << "Printing function:" << func.name() << "\n";
    Internal::FunctionSorter sorter;
    sorter.sort(func);
    std::vector<Func> funcs = sorter.get_functions();
    for (int i = (int)funcs.size() - 1; i >= 0; i--) {
        Func &func = funcs[i];
        Internal::debug(0) << "  funcs[" << i << "]: " << func.name() << "\n";
        for (int update_id = -1; update_id < func.num_update_definitions(); update_id++) {
            if (update_id >= 0) {
                Internal::debug(0) << "    update:" << func.update_value(update_id) << "\n";
            } else {
                Internal::debug(0) << "    init:" << func.value() << "\n";
            }
        }
    }
}

// Testing code
namespace Internal {

void test_simple_bounds_inference() {
    Var x("x"), y("y");
    int height = 32;
    int width = 16;

    Func input("input");
    input(x, y) = 0.0f;
    Func blur_x("blur_x");
    blur_x(x, y) = input(x, y) + input(x+1, y) + input(x+2, y);
    Func blur_y("blur_y");
    blur_y(x, y) = blur_x(x, y) + blur_x(x, y+1) + blur_x(x, y+2);

    RDom r(0, width-2, 0, height-2);
    Expr loss = blur_y(r.x, r.y);

    BoundsInferencer bounds_inferencer;
    bounds_inferencer.inference(loss);
    std::map<FuncKey, RDom> bounds = bounds_inferencer.get_func_bounds();

    FuncKey blur_y_key{blur_y.name(), -1};
    internal_assert(equal(bounds[blur_y_key][0].min(), 0))
        << "Expected 0 instead of " << bounds[blur_y_key][0].min() << "\n" ;
    internal_assert(equal(bounds[blur_y_key][0].extent(), width-2))
        << "Expected " << width-2  << " instead of " << bounds[blur_y_key][0].extent() << "\n" ;
    internal_assert(equal(bounds[blur_y_key][1].min(), 0))
        << "Expected 0 instead of " << bounds[blur_y_key][1].min() << "\n" ;
    internal_assert(equal(bounds[blur_y_key][1].extent(), height-2))
        << "Expected " << height-2  << " instead of " << bounds[blur_y_key][1].extent() << "\n" ;

    FuncKey blur_x_key{blur_x.name(), -1};
    internal_assert(equal(bounds[blur_x_key][0].min(), 0))
        << "Expected 0 instead of " << bounds[blur_x_key][0].min() << "\n" ;
    internal_assert(equal(bounds[blur_x_key][0].extent(), width-2))
        << "Expected " << width-2  << " instead of " << bounds[blur_x_key][0].extent() << "\n" ;
    internal_assert(equal(bounds[blur_x_key][1].min(), 0))
        << "Expected 0 instead of " << bounds[blur_x_key][1].min() << "\n" ;
    internal_assert(equal(bounds[blur_x_key][1].extent(), height))
        << "Expected " << height  << " instead of " << bounds[blur_x_key][1].extent() << "\n" ;

    FuncKey input_key{input.name(), -1};
    internal_assert(equal(bounds[input_key][0].min(), 0))
        << "Expected 0 instead of " << bounds[input_key][0].min() << "\n" ;
    internal_assert(equal(bounds[input_key][0].extent(), width))
        << "Expected " << width  << " instead of " << bounds[input_key][0].extent() << "\n" ;
    internal_assert(equal(bounds[input_key][1].min(), 0))
        << "Expected 0 instead of " << bounds[input_key][1].min() << "\n" ;
    internal_assert(equal(bounds[input_key][1].extent(), height))
        << "Expected " << height  << " instead of " << bounds[input_key][1].extent() << "\n" ;
}

void test_simple_bounds_inference_update() {
    Var x("x");
    Func input("input");
    input(x) = 0.0f;
    Func blur("blur");
    blur(x) = input(x);
    blur(x) += input(x + 1);
    RDom r(0, 2);
    Expr loss = blur(r.x);

    BoundsInferencer bounds_inferencer;
    bounds_inferencer.inference(loss);
    std::map<FuncKey, RDom> bounds = bounds_inferencer.get_func_bounds();

    FuncKey blur_key_1{blur.name(), 0};
    internal_assert(equal(bounds[blur_key_1][0].min(), 0))
        << "Expected 0 instead of " << bounds[blur_key_1][0].min() << "\n" ;
    internal_assert(equal(bounds[blur_key_1][0].extent(), 2))
        << "Expected 2 instead of " << bounds[blur_key_1][0].extent() << "\n" ;
    FuncKey blur_key_0{blur.name(), -1};
    internal_assert(equal(bounds[blur_key_0][0].min(), 0))
        << "Expected 0 instead of " << bounds[blur_key_0][0].min() << "\n" ;
    internal_assert(equal(bounds[blur_key_0][0].extent(), 2))
        << "Expected 2 instead of " << bounds[blur_key_0][0].extent() << "\n" ;
    FuncKey input_key{input.name(), -1};
    internal_assert(equal(bounds[input_key][0].min(), 0))
        << "Expected 0 instead of " << bounds[input_key][0].min() << "\n" ;
    internal_assert(equal(bounds[input_key][0].extent(), 3))
        << "Expected 3 instead of " << bounds[input_key][0].extent() << "\n" ;
}

void test_simple_1d_blur() {
    Var x("x");
    float input_data[] = {1.f, 2.f};
    Buffer<float> input(input_data, 2, "input");
    Func clamped("clamped");
    Expr clamped_x = Halide::clamp(x, 0, input.width() - 1);
    clamped(x) = input(clamped_x);
    Func blur("blur");
    blur(x) = clamped(x) + clamped(x + 1);
    RDom r(0, 2);
    Expr loss = blur(r.x) * blur(r.x);

    std::map<std::string, Func> adjoints = propagate_adjoints(loss);
    Buffer<float> blur_buf = blur.realize(2);
    // d loss / d blur = 2 * blur(x)
    Buffer<float> d_blur_buf = adjoints[blur.name()].realize(2);
    const float eps = 1e-6;
#define CMP(x, target) \
    internal_assert(fabs((x) - (target)) < eps) << \
        "Expected " << (target) << " instead of " << (x) << "\n";

    std::cerr << "d_blur_buf(0):" << d_blur_buf(0) << std::endl;
    std::cerr << "d_blur_buf(1):" << d_blur_buf(1) << std::endl;
    CMP(d_blur_buf(0), 2 * blur_buf(0));
    CMP(d_blur_buf(1), 2 * blur_buf(1));
    Buffer<float> d_clamped_buf = adjoints[clamped.name()].realize(2);
    CMP(d_clamped_buf(0), d_blur_buf(0));
    CMP(d_clamped_buf(1), d_blur_buf(0) + d_blur_buf(1));

#undef CMP
}

void test_simple_2d_blur() {
    Var x("x"), y("y");
    float input_data[] = {
        0.f, 1.f, 0.f, 0.f, 0.f,
        1.f, 1.f, 1.f, 0.f, 0.f,
        0.f, 1.f, 0.f, 0.f, 0.f,
        0.f, 0.f, 0.f, 0.f, 0.f,
        0.f, 0.f, 0.f, 0.f, 0.f
    };
    Buffer<float> input(input_data, 5, 5, "input");
    Func clamped("clamped");
    Expr clamped_x = Halide::clamp(x, 0, input.width()-1);
    Expr clamped_y = Halide::clamp(y, 0, input.height()-1);
    clamped(x, y) = input(clamped_x, clamped_y);
    Func blur_x("blur_x");
    blur_x(x, y) = clamped(x, y) + clamped(x + 1, y) + clamped(x + 2, y);
    Func blur_y("blur_y");
    blur_y(x, y) = blur_x(x, y) + blur_x(x, y + 1) + blur_x(x, y + 2);

    RDom r(0, 5, 0, 5);
    Expr loss = blur_y(r.x, r.y) * blur_y(r.x, r.y);

    std::map<std::string, Func> adjoints = propagate_adjoints(loss);
    Buffer<float> blur_y_buf = blur_y.realize(5, 5);
    // d loss / d blur_y = 2 * blur_y(x, y)
    Buffer<float> d_blur_y_buf = adjoints[blur_y.name()].realize(5, 5);
    const float eps = 1e-6;
    for (int y = 0; y < 5; y++) {
        for (int x = 0; x < 5; x++) {
            float target = 2 * blur_y_buf(x, y);
            float diff = fabs(d_blur_y_buf(x, y) - target);
            internal_assert(diff < eps)
                << "Expected d_blur_y(" << x << ", " << y << ") to be " <<
                    target << " instead of " << d_blur_y_buf(x, y) << "\n" ;
        }
    }
    // d loss / d blur_x = d blur_y(x, y) + d blur_y(x, y - 1) + d blur_y(x, y - 2)
    print_func(adjoints[blur_x.name()]);
    Buffer<float> d_blur_x_buf = adjoints[blur_x.name()].realize(5, 5);
    for (int y = 0; y < 5; y++) {
        for (int x = 0; x < 5; x++) {
            float target = d_blur_y_buf(x, y);
            if (y >= 1) {
                target += d_blur_y_buf(x, y - 1);
            }
            if (y >= 2) {
                target += d_blur_y_buf(x, y - 2);
            }
            float diff = fabs(d_blur_x_buf(x, y) - target);
            internal_assert(diff < eps)
                << "Expected d_blur_x(" << x << ", " << y << ") to be " <<
                target << " instead of " << d_blur_x_buf(x, y) << "\n" ;
        }
    }
    Buffer<float> d_clamped = adjoints[clamped.name()].realize(5, 5);
    // d loss / d clamped = d blur_x(x, y) + d blur_x(x - 1, y) + d blur_x(x - 2, y)
    for (int y = 0; y < 5; y++) {
        for (int x = 0; x < 5; x++) {
            float target = d_blur_x_buf(x, y);
            if (x >= 1) {
                target += d_blur_x_buf(x - 1, y);
            }
            if (x >= 2) {
                target += d_blur_x_buf(x - 2, y);
            }
            float diff = fabs(d_clamped(x, y) - target);
            internal_assert(diff < eps)
                << "Expected d_clamped(" << x << ", " << y << ") to be " <<
                target << " instead of " << d_clamped(x, y) << "\n" ;
        }
    }
}

void test_update() {
    Var x("x");
    float input_data[] = {1.f, 2.f};
    Buffer<float> input(input_data, 2, "input");
    Func clamped("clamped");
    Expr clamped_x = Halide::clamp(x, 0, input.width() - 1);
    clamped(x) = input(clamped_x);
    Func blur("blur");
    blur(x) = clamped(x);
    blur(x) += clamped(x + 1);
    RDom r(0, 2);
    Expr loss = blur(r.x) * blur(r.x);

    std::map<std::string, Func> adjoints = propagate_adjoints(loss);
    Buffer<float> blur_buf = blur.realize(2);
    // d loss / d blur = 2 * blur(x)
    Buffer<float> d_blur_buf = adjoints[blur.name()].realize(2);
    const float eps = 1e-6;
#define CMP(x, target) \
internal_assert(fabs((x) - (target)) < eps) << \
"Expected " << (target) << " instead of " << (x) << "\n";

    CMP(d_blur_buf(0), 2 * blur_buf(0));
    CMP(d_blur_buf(1), 2 * blur_buf(1));
    Buffer<float> d_clamped_buf = adjoints[clamped.name()].realize(2);
    CMP(d_clamped_buf(0), d_blur_buf(0));
    CMP(d_clamped_buf(1), d_blur_buf(0) + d_blur_buf(1));

#undef CMP
}

void derivative_test() {
    test_simple_bounds_inference();
    test_simple_bounds_inference_update();
    test_simple_1d_blur();
    test_simple_2d_blur();
    test_update();
    debug(0) << "Derivative test passed\n";
}

} // namespace Internal


} // namespace Halide
