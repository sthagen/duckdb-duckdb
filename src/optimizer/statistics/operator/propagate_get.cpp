#include "duckdb/common/helper.hpp"
#include "duckdb/optimizer/statistics_propagator.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/filter/conjunction_filter.hpp"
#include "duckdb/planner/filter/constant_filter.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/filter/null_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "duckdb/function/scalar/generic_common.hpp"
#include "duckdb/function/scalar/generic_functions.hpp"

namespace duckdb {

static void GetColumnIndex(unique_ptr<Expression> &expr, idx_t &index) {
	if (expr->type == ExpressionType::BOUND_REF) {
		auto &bound_ref = expr->Cast<BoundReferenceExpression>();
		index = bound_ref.index;
		return;
	}
	ExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<Expression> &child) { GetColumnIndex(child, index); });
}

FilterPropagateResult StatisticsPropagator::PropagateTableFilter(ColumnBinding stats_binding, BaseStatistics &stats,
                                                                 TableFilter &filter) {
	if (filter.filter_type == TableFilterType::EXPRESSION_FILTER) {
		auto &expr_filter = filter.Cast<ExpressionFilter>();

		// get physical storage index of the filter
		// since it is a table filter, every storage index is the same
		idx_t physical_index = DConstants::INVALID_INDEX;
		GetColumnIndex(expr_filter.expr, physical_index);
		D_ASSERT(physical_index != DConstants::INVALID_INDEX);

		auto column_ref = make_uniq<BoundColumnRefExpression>(stats.GetType(), stats_binding);
		auto filter_expr = expr_filter.ToExpression(*column_ref);
		// handle the filter before updating the statistics
		// otherwise the filter can be pruned by the updated statistics
		auto propagate_result = HandleFilter(filter_expr);
		auto colref = make_uniq<BoundReferenceExpression>(stats.GetType(), physical_index);
		UpdateFilterStatistics(*filter_expr);

		// replace BoundColumnRefs with BoundRefs
		ExpressionFilter::ReplaceExpressionRecursive(filter_expr, *colref, ExpressionType::BOUND_COLUMN_REF);
		expr_filter.expr = std::move(filter_expr);
		return propagate_result;
	}
	return filter.CheckStatistics(stats);
}

void StatisticsPropagator::UpdateFilterStatistics(BaseStatistics &input, TableFilter &filter) {
	// FIXME: update stats...
	switch (filter.filter_type) {
	case TableFilterType::CONJUNCTION_AND: {
		auto &conjunction_and = filter.Cast<ConjunctionAndFilter>();
		for (auto &child_filter : conjunction_and.child_filters) {
			UpdateFilterStatistics(input, *child_filter);
		}
		break;
	}
	case TableFilterType::CONSTANT_COMPARISON: {
		auto &constant_filter = filter.Cast<ConstantFilter>();
		UpdateFilterStatistics(input, constant_filter.comparison_type, constant_filter.constant);
		break;
	}
	default:
		break;
	}
}

static bool IsConstantOrNullFilter(TableFilter &table_filter) {
	if (table_filter.filter_type != TableFilterType::EXPRESSION_FILTER) {
		return false;
	}
	auto &expr_filter = table_filter.Cast<ExpressionFilter>();
	if (expr_filter.expr->type != ExpressionType::BOUND_FUNCTION) {
		return false;
	}
	auto &func = expr_filter.expr->Cast<BoundFunctionExpression>();
	return ConstantOrNull::IsConstantOrNull(func, Value::BOOLEAN(true));
}

static bool CanReplaceConstantOrNull(TableFilter &table_filter) {
	if (!IsConstantOrNullFilter(table_filter)) {
		throw InternalException("CanReplaceConstantOrNull() called on unexepected Table Filter");
	}
	D_ASSERT(table_filter.filter_type == TableFilterType::EXPRESSION_FILTER);
	auto &expr_filter = table_filter.Cast<ExpressionFilter>();
	auto &func = expr_filter.expr->Cast<BoundFunctionExpression>();
	if (ConstantOrNull::IsConstantOrNull(func, Value::BOOLEAN(true))) {
		for (auto child = ++func.children.begin(); child != func.children.end(); child++) {
			switch (child->get()->type) {
			case ExpressionType::BOUND_REF:
			case ExpressionType::VALUE_CONSTANT:
				continue;
			default:
				// expression type could be a function like Coalesce
				return false;
			}
		}
	}
	// all children of constant or null are bound refs to the table filter column
	return true;
}

unique_ptr<NodeStatistics> StatisticsPropagator::PropagateStatistics(LogicalGet &get,
                                                                     unique_ptr<LogicalOperator> &node_ptr) {
	if (get.function.cardinality) {
		node_stats = get.function.cardinality(context, get.bind_data.get());
	}
	if (!get.function.statistics) {
		// no column statistics to get
		return std::move(node_stats);
	}
	auto &column_ids = get.GetColumnIds();
	for (idx_t i = 0; i < column_ids.size(); i++) {
		auto stats = get.function.statistics(context, get.bind_data.get(), column_ids[i].GetPrimaryIndex());
		if (stats) {
			ColumnBinding binding(get.table_index, i);
			statistics_map.insert(make_pair(binding, std::move(stats)));
		}
	}
	// push table filters into the statistics
	vector<idx_t> column_indexes;
	column_indexes.reserve(get.table_filters.filters.size());
	for (auto &kv : get.table_filters.filters) {
		column_indexes.push_back(kv.first);
	}

	for (auto &table_filter_column : column_indexes) {
		idx_t column_index;
		for (column_index = 0; column_index < column_ids.size(); column_index++) {
			if (column_ids[column_index].GetPrimaryIndex() == table_filter_column) {
				break;
			}
		}
		D_ASSERT(column_index < column_ids.size());
		D_ASSERT(column_ids[column_index].GetPrimaryIndex() == table_filter_column);

		// find the stats
		ColumnBinding stats_binding(get.table_index, column_index);
		auto entry = statistics_map.find(stats_binding);
		if (entry == statistics_map.end()) {
			// no stats for this entry
			continue;
		}
		auto &stats = *entry->second;

		// fetch the table filter
		D_ASSERT(get.table_filters.filters.count(table_filter_column) > 0);
		auto &filter = get.table_filters.filters[table_filter_column];
		auto propagate_result = PropagateTableFilter(stats_binding, stats, *filter);
		switch (propagate_result) {
		case FilterPropagateResult::FILTER_ALWAYS_TRUE:
			// filter is always true; it is useless to execute it
			// erase this condition
			get.table_filters.filters.erase(table_filter_column);
			break;
		case FilterPropagateResult::FILTER_TRUE_OR_NULL: {
			if (IsConstantOrNullFilter(*get.table_filters.filters[table_filter_column]) &&
			    !CanReplaceConstantOrNull(*get.table_filters.filters[table_filter_column])) {
				break;
			}
			// filter is true or null; we can replace this with a not null filter
			get.table_filters.filters[table_filter_column] = make_uniq<IsNotNullFilter>();
			break;
		}
		case FilterPropagateResult::FILTER_FALSE_OR_NULL:
		case FilterPropagateResult::FILTER_ALWAYS_FALSE:
			// filter is always false; this entire filter should be replaced by an empty result block
			ReplaceWithEmptyResult(node_ptr);
			return make_uniq<NodeStatistics>(0U, 0U);
		default:
			// general case: filter can be true or false, update this columns' statistics
			UpdateFilterStatistics(stats, *filter);
			break;
		}
	}
	return std::move(node_stats);
}

} // namespace duckdb
