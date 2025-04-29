/*
 * Copyright (C) 2019-present ScyllaDB
 *
 * Modified by ScyllaDB
 */
/*
 * SPDX-License-Identifier: (LicenseRef-ScyllaDB-Source-Available-1.0 and Apache-2.0)
 */
#pragma once
#include <seastar/core/semaphore.hh>
#include "service/paxos/proposal.hh"
#include "utils/log.hh"
#include "utils/digest_algorithm.hh"
#include "db/timeout_clock.hh"
#include <unordered_map>
#include "utils/UUID_gen.hh"
#include "service/paxos/prepare_response.hh"
#include "cql3/untyped_result_set.hh"

namespace service {
class storage_proxy;
}
namespace db { class system_keyspace; }

namespace service::paxos {

using clock_type = db::timeout_clock;

class paxos_store;

// The state of a CAS update of a given primary key as persisted in the paxos table.
class paxos_state {
private:
    class guard;

    class key_lock_map {
        using semaphore = basic_semaphore<semaphore_default_exception_factory, clock_type>;
        using semaphore_units = semaphore_units<semaphore_default_exception_factory, clock_type>;
        using map = std::unordered_map<dht::token, semaphore>;

        semaphore& get_semaphore_for_key(const dht::token& key);
        void release_semaphore_for_key(const dht::token& key);

        map _locks;
    public:

        friend class guard;
    };

    class guard {
        key_lock_map& _map;
        dht::token _key;
        clock_type::time_point _timeout;
        key_lock_map::semaphore_units _units;
    public:
        future<> lock () {
            return get_units(_map.get_semaphore_for_key(_key), 1, _timeout).then([this] (auto&& u) { _units = std::move(u); });
        }
        guard(key_lock_map& map, const dht::token& key, clock_type::time_point timeout) : _map(map), _key(key), _timeout(timeout) {};
        guard(guard&& o) = default;
        ~guard() {
            _units.return_all();
            _map.release_semaphore_for_key(_key);
        }
    };

    // Locks are local to the shard which owns the corresponding token range.
    // Protects concurrent reads and writes of the same row in system.paxos table.
    static thread_local key_lock_map _paxos_table_lock;
    // Taken by the coordinator code to allow only one instance of PAXOS to run for each key.
    // This prevents contantion between multiple clients trying to modify the
    // same key through the same coordinator and stealing the ballot from
    // each other.
    static thread_local key_lock_map _coordinator_lock;


    static future<guard> get_replica_lock(const dht::token& key, clock_type::time_point timeout);

    utils::UUID _promised_ballot = utils::UUID_gen::min_time_UUID();
    std::optional<proposal> _accepted_proposal;
    std::optional<proposal> _most_recent_commit;

public:

    static future<guard> get_cas_lock(const dht::token& key, clock_type::time_point timeout);

    static logging::logger logger;

    paxos_state() {}

    paxos_state(utils::UUID promised, std::optional<proposal> accepted, std::optional<proposal> commit)
        : _promised_ballot(std::move(promised))
        , _accepted_proposal(std::move(accepted))
        , _most_recent_commit(std::move(commit)) {}

    static paxos_state from_row(partition_key_view key, schema_ptr s, const cql3::untyped_result_set& result_set);

    // Replica RPC endpoint for Paxos "prepare" phase.
    static future<prepare_response> prepare(storage_proxy& sp, paxos_store& sys_ks, tracing::trace_state_ptr tr_state, schema_ptr schema,
            const query::read_command& cmd, const partition_key& key, utils::UUID ballot,
            bool only_digest, query::digest_algorithm da, clock_type::time_point timeout);
    // Replica RPC endpoint for Paxos "accept" phase.
    static future<bool> accept(storage_proxy& sp, paxos_store& sys_ks, tracing::trace_state_ptr tr_state, schema_ptr schema, dht::token token, const proposal& proposal,
            clock_type::time_point timeout);
    // Replica RPC endpoint for Paxos "learn".
    static future<> learn(storage_proxy& sp, paxos_store& sys_ks, schema_ptr schema, proposal decision, clock_type::time_point timeout, tracing::trace_state_ptr tr_state);
    // Replica RPC endpoint for pruning Paxos table
    static future<> prune(paxos_store& sys_ks, schema_ptr schema, const partition_key& key, utils::UUID ballot, clock_type::time_point timeout,
            tracing::trace_state_ptr tr_state);
};

int32_t paxos_ttl_sec(const schema& s);

class paxos_store {
    gms::feature_service& _features;
    migration_manager& _mm;
    sharded<db::system_keyspace>& _sys_ks;

    future<::shared_ptr<cql3::untyped_result_set>> execute_query(const sstring& query_text,
        db::timeout_clock::time_point timeout,
        const std::vector<data_value_or_unset>& params);

    // Executes the given CQL query on the specified table partition.
    // 
    // The `cql_format` parameter must contain exactly three `{}` placeholders,
    // in this order:
    //   1. Keyspace name
    //   2. Table (column family) name
    //   3. Partition key filter
    //
    // Any '?' placeholders for bind variables (corresponding to the `args` parameter)
    // must appear *before* the partition key filter in the query string.
    //
    // Parameters:
    // - s: Target table schema
    // - key: Partition key to filter on
    // - timeout: Query timeout
    // - args: Bind values corresponding to '?' placeholders (if any)
    template <const std::string_view& cql_format, typename... Args>
    future<::shared_ptr<cql3::untyped_result_set>> execute_on_partition(
        const schema& s,
        partition_key_view key,
        db::timeout_clock::time_point timeout,
        Args&&... args);

    bool check_use_tablets(const schema& s) const;
public:
    explicit paxos_store(gms::feature_service& features, migration_manager& mm, sharded<db::system_keyspace>& sys_ks);
    future<schema_ptr> inject_columns(schema_ptr);
    future<column_mapping> get_column_mapping(table_id, table_schema_version v);
    future<service::paxos::paxos_state> load_paxos_state(partition_key_view key, schema_ptr s, gc_clock::time_point now,
        db::timeout_clock::time_point timeout);
    future<> save_paxos_promise(const schema& s, const partition_key& key, const utils::UUID& ballot, db::timeout_clock::time_point timeout);
    future<> save_paxos_proposal(const schema& s, const service::paxos::proposal& proposal, db::timeout_clock::time_point timeout);
    future<> save_paxos_decision(const schema& s, const service::paxos::proposal& decision, db::timeout_clock::time_point timeout);
    future<> delete_paxos_decision(const schema& s, const partition_key& key, const utils::UUID& ballot, db::timeout_clock::time_point timeout);
};

} // end of namespace "service::paxos"

