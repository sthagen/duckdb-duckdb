#include "duckdb/execution/column_binding_resolver.hpp"

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/operator/logical_any_join.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_create_index.hpp"
#include "duckdb/planner/operator/logical_extension_operator.hpp"
#include "duckdb/planner/operator/logical_insert.hpp"
#include "duckdb/planner/operator/logical_recursive_cte.hpp"

namespace duckdb {

ColumnBindingResolver::ColumnBindingResolver(bool verify_only) : verify_only(verify_only) {
}

void ColumnBindingResolver::VisitOperator(LogicalOperator &op) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_ASOF_JOIN:
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN: {
		// special case: comparison join
		auto &comp_join = op.Cast<LogicalComparisonJoin>();
		// first get the bindings of the LHS and resolve the LHS expressions
		VisitOperator(*comp_join.children[0]);
		for (auto &cond : comp_join.conditions) {
			VisitExpression(&cond.left);
		}
		// visit the duplicate eliminated columns on the LHS, if any
		for (auto &expr : comp_join.duplicate_eliminated_columns) {
			VisitExpression(&expr);
		}
		// then get the bindings of the RHS and resolve the RHS expressions
		VisitOperator(*comp_join.children[1]);
		for (auto &cond : comp_join.conditions) {
			VisitExpression(&cond.right);
		}
		// finally update the bindings with the result bindings of the join
		bindings = op.GetColumnBindings();
		types = op.types;
		// resolve any mixed predicates
		// for now, only ASOF supports this.
		if (comp_join.predicate) {
			D_ASSERT(op.type == LogicalOperatorType::LOGICAL_ASOF_JOIN);
			VisitExpression(&comp_join.predicate);
		}
		return;
	}
	case LogicalOperatorType::LOGICAL_DELIM_JOIN: {
		auto &comp_join = op.Cast<LogicalComparisonJoin>();
		// get bindings from the duplicate-eliminated side
		auto &delim_side = comp_join.delim_flipped ? *comp_join.children[1] : *comp_join.children[0];
		VisitOperator(delim_side);
		for (auto &cond : comp_join.conditions) {
			auto &expr = comp_join.delim_flipped ? cond.right : cond.left;
			VisitExpression(&expr);
		}
		// visit the duplicate eliminated columns
		for (auto &expr : comp_join.duplicate_eliminated_columns) {
			VisitExpression(&expr);
		}
		// now the other side
		auto &other_side = comp_join.delim_flipped ? *comp_join.children[0] : *comp_join.children[1];
		VisitOperator(other_side);
		for (auto &cond : comp_join.conditions) {
			auto &expr = comp_join.delim_flipped ? cond.left : cond.right;
			VisitExpression(&expr);
		}
		// finally update the bindings with the result bindings of the join
		bindings = op.GetColumnBindings();
		types = op.types;
		return;
	}
	case LogicalOperatorType::LOGICAL_ANY_JOIN: {
		// ANY join, this join is different because we evaluate the expression on the bindings of BOTH join sides at
		// once i.e. we set the bindings first to the bindings of the entire join, and then resolve the expressions of
		// this operator
		VisitOperatorChildren(op);
		bindings = op.GetColumnBindings();
		types = op.types;
		auto &any_join = op.Cast<LogicalAnyJoin>();
		if (any_join.join_type == JoinType::SEMI || any_join.join_type == JoinType::ANTI) {
			auto right_bindings = op.children[1]->GetColumnBindings();
			bindings.insert(bindings.end(), right_bindings.begin(), right_bindings.end());
			auto &right_types = op.children[1]->types;
			types.insert(types.end(), right_types.begin(), right_types.end());
		}
		if (any_join.join_type == JoinType::RIGHT_SEMI || any_join.join_type == JoinType::RIGHT_ANTI) {
			throw InternalException("RIGHT SEMI/ANTI any join not supported yet");
		}
		VisitOperatorExpressions(op);
		return;
	}
	case LogicalOperatorType::LOGICAL_CREATE_INDEX: {
		// CREATE INDEX statement, add the columns of the table with table index 0 to the binding set
		// afterwards bind the expressions of the CREATE INDEX statement
		auto &create_index = op.Cast<LogicalCreateIndex>();
		bindings = LogicalOperator::GenerateColumnBindings(0, create_index.table.GetColumns().LogicalColumnCount());
		// TODO: fill types in too (clearing skips type checks)
		types.clear();
		VisitOperatorExpressions(op);
		return;
	}
	case LogicalOperatorType::LOGICAL_GET: {
		//! We first need to update the current set of bindings and then visit operator expressions
		bindings = op.GetColumnBindings();
		types = op.types;
		VisitOperatorExpressions(op);
		return;
	}
	case LogicalOperatorType::LOGICAL_INSERT: {
		//! We want to execute the normal path, but also add a dummy 'excluded' binding if there is a
		// ON CONFLICT DO UPDATE clause
		auto &insert_op = op.Cast<LogicalInsert>();
		if (insert_op.action_type != OnConflictAction::THROW) {
			// Get the bindings from the children
			VisitOperatorChildren(op);
			auto column_count = insert_op.table.GetColumns().PhysicalColumnCount();
			auto dummy_bindings = LogicalOperator::GenerateColumnBindings(insert_op.excluded_table_index, column_count);
			// Now insert our dummy bindings at the start of the bindings,
			// so the first 'column_count' indices of the chunk are reserved for our 'excluded' columns
			bindings.insert(bindings.begin(), dummy_bindings.begin(), dummy_bindings.end());
			// TODO: fill types in too (clearing skips type checks)
			types.clear();
			if (insert_op.on_conflict_condition) {
				VisitExpression(&insert_op.on_conflict_condition);
			}
			if (insert_op.do_update_condition) {
				VisitExpression(&insert_op.do_update_condition);
			}
			VisitOperatorExpressions(op);
			bindings = op.GetColumnBindings();
			types = op.types;
			return;
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_EXTENSION_OPERATOR: {
		auto &ext_op = op.Cast<LogicalExtensionOperator>();
		// Just to be very sure, we clear before and after resolving extension operator column bindings
		// This skips checks, but makes sure we don't break any extension operators with type verification
		types.clear();
		ext_op.ResolveColumnBindings(*this, bindings);
		types.clear();
		return;
	}
	case LogicalOperatorType::LOGICAL_RECURSIVE_CTE: {
		auto &rec = op.Cast<LogicalRecursiveCTE>();
		VisitOperatorChildren(op);
		bindings = op.GetColumnBindings();
		types = op.types;
		for (auto &expr : rec.key_targets) {
			VisitExpression(&expr);
		}
		return;
	}
	default:
		break;
	}

	// general case
	// first visit the children of this operator
	VisitOperatorChildren(op);
	// now visit the expressions of this operator to resolve any bound column references
	VisitOperatorExpressions(op);
	// finally update the current set of bindings to the current set of column bindings
	bindings = op.GetColumnBindings();
	types = op.types;
}

unique_ptr<Expression> ColumnBindingResolver::VisitReplace(BoundColumnRefExpression &expr,
                                                           unique_ptr<Expression> *expr_ptr) {
	D_ASSERT(expr.depth == 0);
	// check the current set of column bindings to see which index corresponds to the column reference
	for (idx_t i = 0; i < bindings.size(); i++) {
		if (expr.binding == bindings[i]) {
			if (!types.empty()) {
				if (bindings.size() != types.size()) {
					throw InternalException(
					    "Failed to bind column reference \"%s\" [%d.%d]: inequal num bindings/types (%llu != %llu)",
					    expr.GetAlias(), expr.binding.table_index, expr.binding.column_index, bindings.size(),
					    types.size());
				}
				if (expr.return_type != types[i]) {
					throw InternalException("Failed to bind column reference \"%s\" [%d.%d]: inequal types (%s != %s)",
					                        expr.GetAlias(), expr.binding.table_index, expr.binding.column_index,
					                        expr.return_type.ToString(), types[i].ToString());
				}
			}
			if (verify_only) {
				// in verification mode
				return nullptr;
			}
			return make_uniq<BoundReferenceExpression>(expr.GetAlias(), expr.return_type, i);
		}
	}
	// LCOV_EXCL_START
	// could not bind the column reference, this should never happen and indicates a bug in the code
	// generate an error message
	throw InternalException("Failed to bind column reference \"%s\" [%d.%d] (bindings: %s)", expr.GetAlias(),
	                        expr.binding.table_index, expr.binding.column_index,
	                        LogicalOperator::ColumnBindingsToString(bindings));
	// LCOV_EXCL_STOP
}

unordered_set<idx_t> ColumnBindingResolver::VerifyInternal(LogicalOperator &op) {
	unordered_set<idx_t> result;
	for (auto &child : op.children) {
		auto child_indexes = VerifyInternal(*child);
		for (auto index : child_indexes) {
			D_ASSERT(index != DConstants::INVALID_INDEX);
			if (result.find(index) != result.end()) {
				throw InternalException("Duplicate table index \"%lld\" found", index);
			}
			result.insert(index);
		}
	}
	auto indexes = op.GetTableIndex();
	for (auto index : indexes) {
		D_ASSERT(index != DConstants::INVALID_INDEX);
		if (result.find(index) != result.end()) {
			throw InternalException("Duplicate table index \"%lld\" found", index);
		}
		result.insert(index);
	}
	return result;
}

void ColumnBindingResolver::Verify(LogicalOperator &op) {
#ifdef DEBUG
	op.ResolveOperatorTypes();
	ColumnBindingResolver resolver(true);
	resolver.VisitOperator(op);
	VerifyInternal(op);
#endif
}

} // namespace duckdb
