//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/query_profiler.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/deque.hpp"
#include "duckdb/common/enums/profiler_format.hpp"
#include "duckdb/common/enums/explain_format.hpp"
#include "duckdb/common/pair.hpp"
#include "duckdb/common/profiler.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/winapi.hpp"
#include "duckdb/execution/expression_executor_state.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/main/profiling_info.hpp"
#include "duckdb/main/profiling_node.hpp"

#include <stack>

namespace duckdb {

class ClientContext;
class ExpressionExecutor;
class ProfilingNode;
class PhysicalOperator;
class SQLStatement;

enum class ProfilingCoverage : uint8_t { SELECT = 0, ALL = 1 };

struct OperatorInformation {
	explicit OperatorInformation() {
	}

	string name;

	double time = 0;
	idx_t elements_returned = 0;
	idx_t result_set_size = 0;
	idx_t system_peak_buffer_manager_memory = 0;
	idx_t system_peak_temp_directory_size = 0;

	InsertionOrderPreservingMap<string> extra_info;

	void AddTime(double n_time) {
		time += n_time;
	}

	void AddReturnedElements(idx_t n_elements) {
		elements_returned += n_elements;
	}

	void AddResultSetSize(idx_t n_result_set_size) {
		result_set_size += n_result_set_size;
	}

	void UpdateSystemPeakBufferManagerMemory(idx_t used_memory) {
		if (used_memory > system_peak_buffer_manager_memory) {
			system_peak_buffer_manager_memory = used_memory;
		}
	}

	void UpdateSystemPeakTempDirectorySize(idx_t used_swap) {
		if (used_swap > system_peak_temp_directory_size) {
			system_peak_temp_directory_size = used_swap;
		}
	}
};

//! The OperatorProfiler measures timings of individual operators
//! This class exists once for all operators and collects `OperatorInfo` for each operator
class OperatorProfiler {
	friend class QueryProfiler;

public:
	DUCKDB_API explicit OperatorProfiler(ClientContext &context);
	~OperatorProfiler() {
	}

public:
	DUCKDB_API void StartOperator(optional_ptr<const PhysicalOperator> phys_op);
	DUCKDB_API void EndOperator(optional_ptr<DataChunk> chunk);
	DUCKDB_API void FinishSource(GlobalSourceState &gstate, LocalSourceState &lstate);

	//! Adds the timings in the OperatorProfiler (tree) to the QueryProfiler (tree).
	DUCKDB_API void Flush(const PhysicalOperator &phys_op);
	DUCKDB_API OperatorInformation &GetOperatorInfo(const PhysicalOperator &phys_op);
	DUCKDB_API bool OperatorInfoIsInitialized(const PhysicalOperator &phys_op);
	DUCKDB_API void AddExtraInfo(InsertionOrderPreservingMap<string> extra_info);

public:
	ClientContext &context;

private:
	//! Whether or not the profiler is enabled
	bool enabled;
	//! Sub-settings for the operator profiler
	profiler_settings_t settings;

	//! The timer used to time the execution time of the individual Physical Operators
	Profiler op;
	//! The stack of Physical Operators that are currently active
	optional_ptr<const PhysicalOperator> active_operator;
	//! A mapping of physical operators to profiled operator information.
	reference_map_t<const PhysicalOperator, OperatorInformation> operator_infos;
};

struct QueryInfo {
	QueryInfo() {
	}
	string query_name;
	ProfilingInfo query_global_info;
};

//! The QueryProfiler can be used to measure timings of queries
class QueryProfiler {
public:
	DUCKDB_API explicit QueryProfiler(ClientContext &context);

public:
	// Propagate save_location, enabled, detailed_enabled and automatic_print_format.
	void Propagate(QueryProfiler &qp);

	using TreeMap = reference_map_t<const PhysicalOperator, reference<ProfilingNode>>;

private:
	unique_ptr<ProfilingNode> CreateTree(const PhysicalOperator &root, const profiler_settings_t &settings,
	                                     const idx_t depth = 0);
	void Render(const ProfilingNode &node, std::ostream &str) const;
	string RenderDisabledMessage(ProfilerPrintFormat format) const;

public:
	DUCKDB_API bool IsEnabled() const;
	DUCKDB_API bool IsDetailedEnabled() const;
	DUCKDB_API ProfilerPrintFormat GetPrintFormat(ExplainFormat format = ExplainFormat::DEFAULT) const;
	DUCKDB_API bool PrintOptimizerOutput() const;
	DUCKDB_API string GetSaveLocation() const;

	DUCKDB_API static QueryProfiler &Get(ClientContext &context);

	DUCKDB_API void Start(const string &query);
	DUCKDB_API void Reset();
	DUCKDB_API void StartQuery(const string &query, bool is_explain_analyze = false, bool start_at_optimizer = false);
	DUCKDB_API void EndQuery();

	DUCKDB_API void StartExplainAnalyze();

	//! Adds the timings gathered by an OperatorProfiler to this query profiler
	DUCKDB_API void Flush(OperatorProfiler &profiler);
	//! Adds the top level query information to the global profiler.
	DUCKDB_API void SetInfo(const double &blocked_thread_time);

	DUCKDB_API void StartPhase(MetricsType phase_metric);
	DUCKDB_API void EndPhase();

	DUCKDB_API void Initialize(const PhysicalOperator &root);

	DUCKDB_API string QueryTreeToString() const;
	DUCKDB_API void QueryTreeToStream(std::ostream &str) const;
	DUCKDB_API void Print();

	//! return the printed as a string. Unlike ToString, which is always formatted as a string,
	//! the return value is formatted based on the current print format (see GetPrintFormat()).
	DUCKDB_API string ToString(ExplainFormat format = ExplainFormat::DEFAULT) const;
	DUCKDB_API string ToString(ProfilerPrintFormat format) const;

	static InsertionOrderPreservingMap<string> JSONSanitize(const InsertionOrderPreservingMap<string> &input);
	static string JSONSanitize(const string &text);
	static string DrawPadded(const string &str, idx_t width);
	DUCKDB_API string ToJSON() const;
	DUCKDB_API void WriteToFile(const char *path, string &info) const;

	idx_t OperatorSize() {
		return tree_map.size();
	}

	void Finalize(ProfilingNode &node);

	//! Return the root of the query tree
	optional_ptr<ProfilingNode> GetRoot() {
		return root.get();
	}

	//! Provides access to the root of the query tree, but ensures there are no concurrent modifications
	//! This can be useful when implementing continuous profiling or making customizations
	DUCKDB_API void GetRootUnderLock(const std::function<void(optional_ptr<ProfilingNode>)> &callback) {
		lock_guard<std::mutex> guard(lock);
		callback(GetRoot());
	}

private:
	ClientContext &context;

	//! Whether or not the query profiler is running
	bool running;
	//! The lock used for accessing the global query profiler or flushing information to it from a thread
	mutable std::mutex lock;

	//! Whether or not the query requires profiling
	bool query_requires_profiling;

	//! The root of the query tree
	unique_ptr<ProfilingNode> root;

	//! Top level query information.
	QueryInfo query_info;
	//! The timer used to time the execution time of the entire query
	Profiler main_query;
	//! A map of a Physical Operator pointer to a tree node
	TreeMap tree_map;
	//! Whether or not we are running as part of a explain_analyze query
	bool is_explain_analyze;

public:
	const TreeMap &GetTreeMap() const {
		return tree_map;
	}

private:
	//! The timer used to time the individual phases of the planning process
	Profiler phase_profiler;
	//! A mapping of the phase names to the timings
	using PhaseTimingStorage = unordered_map<MetricsType, double, MetricsTypeHashFunction>;
	PhaseTimingStorage phase_timings;
	using PhaseTimingItem = PhaseTimingStorage::value_type;
	//! The stack of currently active phases
	vector<MetricsType> phase_stack;

private:
	void MoveOptimizerPhasesToRoot();

	//! Check whether or not an operator type requires query profiling. If none of the ops in a query require profiling
	//! no profiling information is output.
	bool OperatorRequiresProfiling(const PhysicalOperatorType op_type);
	ExplainFormat GetExplainFormat(ProfilerPrintFormat format) const;
};

} // namespace duckdb
