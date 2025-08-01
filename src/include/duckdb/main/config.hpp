//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/config.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/arrow/arrow_type_extension.hpp"

#include "duckdb/common/allocator.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/cgroups.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/encryption_state.hpp"
#include "duckdb/common/enums/access_mode.hpp"
#include "duckdb/common/enums/thread_pin_mode.hpp"
#include "duckdb/common/enums/compression_type.hpp"
#include "duckdb/common/enums/optimizer_type.hpp"
#include "duckdb/common/enums/order_type.hpp"
#include "duckdb/common/enums/set_scope.hpp"
#include "duckdb/common/enums/window_aggregation_mode.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/set.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/winapi.hpp"
#include "duckdb/execution/index/index_type_set.hpp"
#include "duckdb/function/cast/default_casts.hpp"
#include "duckdb/function/replacement_scan.hpp"
#include "duckdb/main/client_properties.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/parser/parsed_data/create_info.hpp"
#include "duckdb/parser/parser_extension.hpp"
#include "duckdb/planner/operator_extension.hpp"
#include "duckdb/storage/compression/bitpacking.hpp"
#include "duckdb/function/encoding_function.hpp"
#include "duckdb/logging/log_manager.hpp"
#include "duckdb/common/enums/debug_vector_verification.hpp"
#include "duckdb/logging/logging.hpp"

namespace duckdb {

class BufferManager;
class BufferPool;
class CastFunctionSet;
class CollationBinding;
class ClientContext;
class ErrorManager;
class CompressionFunction;
class TableFunctionRef;
class OperatorExtension;
class StorageExtension;
class ExtensionCallback;
class SecretManager;
class CompressionInfo;
class EncryptionUtil;
class HTTPUtil;

struct CompressionFunctionSet;
struct DatabaseCacheEntry;
struct DBConfig;

enum class CheckpointAbort : uint8_t {
	NO_ABORT = 0,
	DEBUG_ABORT_BEFORE_TRUNCATE = 1,
	DEBUG_ABORT_BEFORE_HEADER = 2,
	DEBUG_ABORT_AFTER_FREE_LIST_WRITE = 3
};

typedef void (*set_global_function_t)(DatabaseInstance *db, DBConfig &config, const Value &parameter);
typedef void (*set_local_function_t)(ClientContext &context, const Value &parameter);
typedef void (*reset_global_function_t)(DatabaseInstance *db, DBConfig &config);
typedef void (*reset_local_function_t)(ClientContext &context);
typedef Value (*get_setting_function_t)(const ClientContext &context);

struct ConfigurationOption {
	const char *name;
	const char *description;
	const char *parameter_type;
	set_global_function_t set_global;
	set_local_function_t set_local;
	reset_global_function_t reset_global;
	reset_local_function_t reset_local;
	get_setting_function_t get_setting;
};

typedef void (*set_option_callback_t)(ClientContext &context, SetScope scope, Value &parameter);

struct ExtensionOption {
	// NOLINTNEXTLINE: work around bug in clang-tidy
	ExtensionOption(string description_p, LogicalType type_p, set_option_callback_t set_function_p,
	                Value default_value_p)
	    : description(std::move(description_p)), type(std::move(type_p)), set_function(set_function_p),
	      default_value(std::move(default_value_p)) {
	}

	string description;
	LogicalType type;
	set_option_callback_t set_function;
	Value default_value;
};

class SerializationCompatibility {
public:
	static SerializationCompatibility FromDatabase(AttachedDatabase &db);
	static SerializationCompatibility FromIndex(idx_t serialization_version);
	static SerializationCompatibility FromString(const string &input);
	static SerializationCompatibility Default();
	static SerializationCompatibility Latest();

public:
	bool Compare(idx_t property_version) const;

public:
	//! The user provided version
	string duckdb_version;
	//! The max version that should be serialized
	idx_t serialization_version;
	//! Whether this was set by a manual SET/PRAGMA or default
	bool manually_set;

protected:
	SerializationCompatibility() = default;
};

struct DBConfigOptions {
	//! Database file path. May be empty for in-memory mode
	string database_path;
	//! Database type. If empty, automatically extracted from `database_path`, where a `type:path` syntax is expected
	string database_type;
	//! Access mode of the database (AUTOMATIC, READ_ONLY or READ_WRITE)
	AccessMode access_mode = AccessMode::AUTOMATIC;
	//! Checkpoint when WAL reaches this size (default: 16MB)
	idx_t checkpoint_wal_size = 1 << 24;
	//! Whether or not to use Direct IO, bypassing operating system buffers
	bool use_direct_io = false;
	//! Whether extensions should be loaded on start-up
	bool load_extensions = true;
#ifdef DUCKDB_EXTENSION_AUTOLOAD_DEFAULT
	//! Whether known extensions are allowed to be automatically loaded when a query depends on them
	bool autoload_known_extensions = DUCKDB_EXTENSION_AUTOLOAD_DEFAULT;
#else
	bool autoload_known_extensions = false;
#endif
#ifdef DUCKDB_EXTENSION_AUTOINSTALL_DEFAULT
	//! Whether known extensions are allowed to be automatically installed when a query depends on them
	bool autoinstall_known_extensions = DUCKDB_EXTENSION_AUTOINSTALL_DEFAULT;
#else
	bool autoinstall_known_extensions = false;
#endif
	//! Override for the default extension repository
	string custom_extension_repo = "";
	//! Override for the default autoload extension repository
	string autoinstall_extension_repo = "";
	//! The maximum memory used by the database system (in bytes). Default: 80% of System available memory
	idx_t maximum_memory = DConstants::INVALID_INDEX;
	//! The maximum size of the 'temp_directory' folder when set (in bytes). Default: 90% of available disk space.
	idx_t maximum_swap_space = DConstants::INVALID_INDEX;
	//! The maximum amount of CPU threads used by the database system. Default: all available.
	idx_t maximum_threads = DConstants::INVALID_INDEX;
	//! The number of external threads that work on DuckDB tasks. Default: 1.
	//! Must be smaller or equal to maximum_threads.
	idx_t external_threads = 1;
	//! Whether or not to create and use a temporary directory to store intermediates that do not fit in memory
	bool use_temporary_directory = true;
	//! Directory to store temporary structures that do not fit in memory
	string temporary_directory;
	//! Whether or not to invoke filesystem trim on free blocks after checkpoint. This will reclaim
	//! space for sparse files, on platforms that support it.
	bool trim_free_blocks = false;
	//! Record timestamps of buffer manager unpin() events. Usable by custom eviction policies.
	bool buffer_manager_track_eviction_timestamps = false;
	//! Whether or not to allow printing unredacted secrets
	bool allow_unredacted_secrets = false;
	//! The collation type of the database
	string collation = string();
	//! The order type used when none is specified (default: ASC)
	OrderType default_order_type = OrderType::ASCENDING;
	//! Disables invalidating the database instance when encountering a fatal error.
	bool disable_database_invalidation = false;
	//! NULL ordering used when none is specified (default: NULLS LAST)
	DefaultOrderByNullType default_null_order = DefaultOrderByNullType::NULLS_LAST;
	//! enable COPY and related commands
	bool enable_external_access = true;
	//! Whether or not the global http metadata cache is used
	bool http_metadata_cache_enable = false;
	//! HTTP Proxy config as 'hostname:port'
	string http_proxy;
	//! HTTP Proxy username for basic auth
	string http_proxy_username;
	//! HTTP Proxy password for basic auth
	string http_proxy_password;
	//! Force checkpoint when CHECKPOINT is called or on shutdown, even if no changes have been made
	bool force_checkpoint = false;
	//! Run a checkpoint on successful shutdown and delete the WAL, to leave only a single database file behind
	bool checkpoint_on_shutdown = true;
	//! Serialize the metadata on checkpoint with compatibility for a given DuckDB version.
	SerializationCompatibility serialization_compatibility = SerializationCompatibility::Default();
	//! Debug flag that decides when a checkpoing should be aborted. Only used for testing purposes.
	CheckpointAbort checkpoint_abort = CheckpointAbort::NO_ABORT;
	//! Initialize the database with the standard set of DuckDB functions
	//! You should probably not touch this unless you know what you are doing
	bool initialize_default_database = true;
	//! The set of disabled optimizers (default empty)
	set<OptimizerType> disabled_optimizers;
	//! The average string length required to use ZSTD compression.
	uint64_t zstd_min_string_length = 4096;
	//! Force a specific compression method to be used when checkpointing (if available)
	CompressionType force_compression = CompressionType::COMPRESSION_AUTO;
	//! The set of disabled compression methods (default empty)
	set<CompressionType> disabled_compression_methods;
	//! Force a specific bitpacking mode to be used when using the bitpacking compression method
	BitpackingMode force_bitpacking_mode = BitpackingMode::AUTO;
	//! Debug setting for window aggregation mode: (window, combine, separate)
	WindowAggregationMode window_mode = WindowAggregationMode::WINDOW;
	//! Whether preserving insertion order should be preserved
	bool preserve_insertion_order = true;
	//! Whether Arrow Arrays use Large or Regular buffers
	ArrowOffsetSize arrow_offset_size = ArrowOffsetSize::REGULAR;
	//! Whether LISTs should produce Arrow ListViews
	bool arrow_use_list_view = false;
	//! For DuckDB types without an obvious corresponding Arrow type, export to an Arrow extension type instead of a
	//! more portable but less efficient format. For example, UUIDs are exported to UTF-8 (string) when false, and
	//! arrow.uuid type when true.
	bool arrow_lossless_conversion = false;
	//! Whether when producing arrow objects we produce string_views or regular strings
	bool produce_arrow_string_views = false;
	//! Database configuration variables as controlled by SET
	case_insensitive_map_t<Value> set_variables;
	//! Database configuration variable default values;
	case_insensitive_map_t<Value> set_variable_defaults;
	//! Directory to store extension binaries in
	string extension_directory;
	//! Whether unsigned extensions should be loaded
	bool allow_unsigned_extensions = false;
	//! Whether community extensions should be loaded
	bool allow_community_extensions = true;
	//! Whether extensions with missing metadata should be loaded
	bool allow_extensions_metadata_mismatch = false;
	//! Enable emitting FSST Vectors
	bool enable_fsst_vectors = false;
	//! Enable VIEWs to create dependencies
	bool enable_view_dependencies = false;
	//! Enable macros to create dependencies
	bool enable_macro_dependencies = false;
	//! Start transactions immediately in all attached databases - instead of lazily when a database is referenced
	bool immediate_transaction_mode = false;
	//! Debug setting - how to initialize  blocks in the storage layer when allocating
	DebugInitialize debug_initialize = DebugInitialize::NO_INITIALIZE;
	//! The set of user-provided options
	case_insensitive_map_t<Value> user_options;
	//! The set of unrecognized (other) options
	case_insensitive_map_t<Value> unrecognized_options;
	//! Whether or not the configuration settings can be altered
	bool lock_configuration = false;
	//! Whether to print bindings when printing the plan (debug mode only)
	static bool debug_print_bindings; // NOLINT: debug setting
	//! The peak allocation threshold at which to flush the allocator after completing a task (1 << 27, ~128MB)
	idx_t allocator_flush_threshold = 134217728ULL;
	//! If bulk deallocation larger than this occurs, flush outstanding allocations (1 << 30, ~1GB)
	idx_t allocator_bulk_deallocation_flush_threshold = 536870912ULL;
	//! Whether the allocator background thread is enabled
	bool allocator_background_threads = false;
	//! DuckDB API surface
	string duckdb_api;
	//! Metadata from DuckDB callers
	string custom_user_agent;
	//! Use old implicit casting style (i.e. allow everything to be implicitly casted to VARCHAR)
	bool old_implicit_casting = false;
	//!  By default, WAL is encrypted for encrypted databases
	bool wal_encryption = true;
	//! Encrypt the temp files
	bool temp_file_encryption = false;
	//! The default block allocation size for new duckdb database files (new as-in, they do not yet exist).
	idx_t default_block_alloc_size = DEFAULT_BLOCK_ALLOC_SIZE;
	//! The default block header size for new duckdb database files.
	idx_t default_block_header_size = DUCKDB_BLOCK_HEADER_STORAGE_SIZE;
	//!  Whether or not to abort if a serialization exception is thrown during WAL playback (when reading truncated WAL)
	bool abort_on_wal_failure = false;
	//! The index_scan_percentage sets a threshold for index scans.
	//! If fewer than MAX(index_scan_max_count, index_scan_percentage * total_row_count)
	//! rows match, we perform an index scan instead of a table scan.
	double index_scan_percentage = 0.001;
	//! The index_scan_max_count sets a threshold for index scans.
	//! If fewer than MAX(index_scan_max_count, index_scan_percentage * total_row_count)
	//! rows match, we perform an index scan instead of a table scan.
	idx_t index_scan_max_count = STANDARD_VECTOR_SIZE;
	//! The maximum number of schemas we will look through for "did you mean..." style errors in the catalog
	idx_t catalog_error_max_schemas = 100;
	//!  Whether or not to always write to the WAL file, even if this is not required
	bool debug_skip_checkpoint_on_commit = false;
	//! Vector verification to enable (debug setting only)
	DebugVectorVerification debug_verify_vector = DebugVectorVerification::NONE;
	//! The maximum amount of vacuum tasks to schedule during a checkpoint
	idx_t max_vacuum_tasks = 100;
	//! Paths that are explicitly allowed, even if enable_external_access is false
	unordered_set<string> allowed_paths;
	//! Directories that are explicitly allowed, even if enable_external_access is false
	set<string> allowed_directories;
	//! The log configuration
	LogConfig log_config = LogConfig();
	//! Whether to enable external file caching using CachingFileSystem
	bool enable_external_file_cache = true;
	//! Output version of arrow depending on the format version
	ArrowFormatVersion arrow_output_version = V1_0;
	//! Partially process tasks before rescheduling - allows for more scheduler fairness between separate queries
#ifdef DUCKDB_ALTERNATIVE_VERIFY
	bool scheduler_process_partial = true;
#else
	bool scheduler_process_partial = false;
#endif
	//! Whether to pin threads to cores (linux only, default AUTOMATIC: on when there are more than 64 cores)
	ThreadPinMode pin_threads = ThreadPinMode::AUTO;
	//! Enable the Parquet reader to identify a Variant group structurally
	bool variant_legacy_encoding = false;

	bool operator==(const DBConfigOptions &other) const;
};

struct DBConfig {
	friend class DatabaseInstance;
	friend class StorageManager;

public:
	DUCKDB_API DBConfig();
	explicit DUCKDB_API DBConfig(bool read_only);
	DUCKDB_API DBConfig(const case_insensitive_map_t<Value> &config_dict, bool read_only);
	DUCKDB_API ~DBConfig();

	mutex config_lock;
	//! Replacement table scans are automatically attempted when a table name cannot be found in the schema
	vector<ReplacementScan> replacement_scans;

	//! Extra parameters that can be SET for loaded extensions
	case_insensitive_map_t<ExtensionOption> extension_parameters;
	//! The FileSystem to use, can be overwritten to allow for injecting custom file systems for testing purposes (e.g.
	//! RamFS or something similar)
	unique_ptr<FileSystem> file_system;
	//! Secret manager
	unique_ptr<SecretManager> secret_manager;
	//! The allocator used by the system
	unique_ptr<Allocator> allocator;
	//! Database configuration options
	DBConfigOptions options;
	//! Extensions made to the parser
	vector<ParserExtension> parser_extensions;
	//! Extensions made to the optimizer
	vector<OptimizerExtension> optimizer_extensions;
	//! Error manager
	unique_ptr<ErrorManager> error_manager;
	//! A reference to the (shared) default allocator (Allocator::DefaultAllocator)
	shared_ptr<Allocator> default_allocator;
	//! Extensions made to binder
	vector<unique_ptr<OperatorExtension>> operator_extensions;
	//! Extensions made to storage
	case_insensitive_map_t<duckdb::unique_ptr<StorageExtension>> storage_extensions;
	//! A buffer pool can be shared across multiple databases (if desired).
	shared_ptr<BufferPool> buffer_pool;
	//! Provide a custom buffer manager implementation (if desired).
	shared_ptr<BufferManager> buffer_manager;
	//! Set of callbacks that can be installed by extensions
	vector<unique_ptr<ExtensionCallback>> extension_callbacks;
	//! Encryption Util for OpenSSL
	shared_ptr<EncryptionUtil> encryption_util;
	//! HTTP Request utility functions
	shared_ptr<HTTPUtil> http_util;
	//! Reference to the database cache entry (if any)
	shared_ptr<DatabaseCacheEntry> db_cache_entry;

public:
	DUCKDB_API static DBConfig &GetConfig(ClientContext &context);
	DUCKDB_API static DBConfig &GetConfig(DatabaseInstance &db);
	DUCKDB_API static DBConfig &Get(AttachedDatabase &db);
	DUCKDB_API static const DBConfig &GetConfig(const ClientContext &context);
	DUCKDB_API static const DBConfig &GetConfig(const DatabaseInstance &db);
	DUCKDB_API static vector<ConfigurationOption> GetOptions();
	DUCKDB_API static idx_t GetOptionCount();
	DUCKDB_API static vector<string> GetOptionNames();
	DUCKDB_API static bool IsInMemoryDatabase(const char *database_path);

	DUCKDB_API void AddExtensionOption(const string &name, string description, LogicalType parameter,
	                                   const Value &default_value = Value(), set_option_callback_t function = nullptr);
	//! Fetch an option by index. Returns a pointer to the option, or nullptr if out of range
	DUCKDB_API static optional_ptr<const ConfigurationOption> GetOptionByIndex(idx_t index);
	//! Fetch an option by name. Returns a pointer to the option, or nullptr if none exists.
	DUCKDB_API static optional_ptr<const ConfigurationOption> GetOptionByName(const string &name);
	DUCKDB_API void SetOption(const ConfigurationOption &option, const Value &value);
	DUCKDB_API void SetOption(DatabaseInstance *db, const ConfigurationOption &option, const Value &value);
	DUCKDB_API void SetOptionByName(const string &name, const Value &value);
	DUCKDB_API void SetOptionsByName(const case_insensitive_map_t<Value> &values);
	DUCKDB_API void ResetOption(DatabaseInstance *db, const ConfigurationOption &option);
	DUCKDB_API void SetOption(const string &name, Value value);
	DUCKDB_API void ResetOption(const string &name);
	static LogicalType ParseLogicalType(const string &type);

	DUCKDB_API void CheckLock(const string &name);

	DUCKDB_API static idx_t ParseMemoryLimit(const string &arg);

	//! Returns the list of possible compression functions for the physical type.
	DUCKDB_API vector<reference<CompressionFunction>> GetCompressionFunctions(const PhysicalType physical_type);
	//! Returns the compression function matching the compression and physical type.
	DUCKDB_API optional_ptr<CompressionFunction> GetCompressionFunction(CompressionType type,
	                                                                    const PhysicalType physical_type);

	//! Returns the encode function matching the encoding name.
	DUCKDB_API optional_ptr<EncodingFunction> GetEncodeFunction(const string &name) const;
	DUCKDB_API void RegisterEncodeFunction(const EncodingFunction &function) const;
	//! Returns the encode function names.
	DUCKDB_API vector<reference<EncodingFunction>> GetLoadedEncodedFunctions() const;
	//! Returns the encode function matching the encoding name.
	DUCKDB_API ArrowTypeExtension GetArrowExtension(ArrowExtensionMetadata info) const;
	DUCKDB_API ArrowTypeExtension GetArrowExtension(const LogicalType &type) const;
	DUCKDB_API bool HasArrowExtension(const LogicalType &type) const;
	DUCKDB_API bool HasArrowExtension(ArrowExtensionMetadata info) const;
	DUCKDB_API void RegisterArrowExtension(const ArrowTypeExtension &extension) const;

	bool operator==(const DBConfig &other);
	bool operator!=(const DBConfig &other);

	DUCKDB_API CastFunctionSet &GetCastFunctions();
	DUCKDB_API CollationBinding &GetCollationBinding();
	DUCKDB_API IndexTypeSet &GetIndexTypes();
	static idx_t GetSystemMaxThreads(FileSystem &fs);
	static idx_t GetSystemAvailableMemory(FileSystem &fs);
	static optional_idx ParseMemoryLimitSlurm(const string &arg);
	void SetDefaultMaxMemory();
	void SetDefaultTempDirectory();

	OrderType ResolveOrder(OrderType order_type) const;
	OrderByNullType ResolveNullOrder(OrderType order_type, OrderByNullType null_type) const;
	const string UserAgent() const;

	template <class OP>
	typename OP::RETURN_TYPE GetSetting(const ClientContext &context) {
		std::lock_guard<mutex> lock(config_lock);
		return OP::GetSetting(context).template GetValue<typename OP::RETURN_TYPE>();
	}

	template <class OP>
	Value GetSettingValue(const ClientContext &context) {
		std::lock_guard<mutex> lock(config_lock);
		return OP::GetSetting(context);
	}

	bool CanAccessFile(const string &path, FileType type);
	void AddAllowedDirectory(const string &path);
	void AddAllowedPath(const string &path);
	string SanitizeAllowedPath(const string &path) const;

private:
	unique_ptr<CompressionFunctionSet> compression_functions;
	unique_ptr<EncodingFunctionSet> encoding_functions;
	unique_ptr<ArrowTypeExtensionSet> arrow_extensions;
	unique_ptr<CastFunctionSet> cast_functions;
	unique_ptr<CollationBinding> collation_bindings;
	unique_ptr<IndexTypeSet> index_types;
	bool is_user_config = true;
};

} // namespace duckdb
