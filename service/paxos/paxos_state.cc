/*
 * Copyright (C) 2019-present ScyllaDB
 *
 * Modified by ScyllaDB
 */

/*
 * SPDX-License-Identifier: LicenseRef-ScyllaDB-Source-Available-1.0
 */
#include <exception>
#include <seastar/core/coroutine.hh>
#include <seastar/coroutine/all.hh>
#include <seastar/coroutine/exception.hh>
#include "service/storage_proxy.hh"
#include "service/paxos/proposal.hh"
#include "service/paxos/paxos_state.hh"
#include "db/system_keyspace.hh"
#include "replica/database.hh"

#include "utils/error_injection.hh"
#include <fmt/format.h>
#include <fmt/ranges.h>

#include "db/schema_tables.hh"
#include "service/migration_manager.hh"
#include "schema/internal_column_set.hh"
#include "cql3/query_processor.hh"
#include "gms/feature_service.hh"

#include "idl/frozen_mutation.dist.hh"
#include "idl/frozen_mutation.dist.impl.hh"

namespace {
    struct pk_filter_printer {
        const schema& s;
    };
}

template <>
struct fmt::formatter<pk_filter_printer>: fmt::formatter<string_view>{
    auto format(const pk_filter_printer& printer, fmt::format_context& ctx) const {
        bool is_first = true;
        auto out = ctx.out();
        for (const auto& c: printer.s.partition_key_columns()) {
            if (is_first) {
                is_first = false;
            } else {
                out = fmt::format_to(out, ",");
            }
            out = fmt::format_to(out, "{}=?", c.name());
        }
        return out;
    }
};

namespace service::paxos {

logging::logger paxos_state::logger("paxos");
thread_local paxos_state::key_lock_map paxos_state::_paxos_table_lock;
thread_local paxos_state::key_lock_map paxos_state::_coordinator_lock;

paxos_state::key_lock_map::semaphore& paxos_state::key_lock_map::get_semaphore_for_key(const dht::token& key) {
    return _locks.try_emplace(key, 1).first->second;
}

void paxos_state::key_lock_map::release_semaphore_for_key(const dht::token& key) {
    auto it = _locks.find(key);
    if (it != _locks.end() && (*it).second.current() == 1) {
        _locks.erase(it);
    }
}

future<paxos_state::guard> paxos_state::get_replica_lock(const dht::token& key, clock_type::time_point timeout) {
    guard m(_paxos_table_lock, key, timeout);
    co_await m.lock();
    co_return m;
}

future<paxos_state::guard> paxos_state::get_cas_lock(const dht::token& key, clock_type::time_point timeout) {
    guard m(_coordinator_lock, key, timeout);
    co_await m.lock();
    co_return m;
}

paxos_state paxos_state::from_row(partition_key_view key, schema_ptr s, const cql3::untyped_result_set& result_set) {
    if (result_set.empty()) {
        return service::paxos::paxos_state();
    }
    auto& row = result_set.one();
    auto promised = row.has("promise")
                    ? row.get_as<utils::UUID>("promise") : utils::UUID_gen::min_time_UUID();

    std::optional<service::paxos::proposal> accepted;
    if (row.has("proposal")) {
        accepted = service::paxos::proposal(row.get_as<utils::UUID>("proposal_ballot"),
                ser::deserialize_from_buffer<>(row.get_blob("proposal"),  std::type_identity<frozen_mutation>(), 0));
    }

    std::optional<service::paxos::proposal> most_recent;
    if (row.has("most_recent_commit_at")) {
        // the value can be missing if it was pruned, supply empty one since
        // it will not going to be used anyway
        auto fm = row.has("most_recent_commit")
            ? ser::deserialize_from_buffer<>(row.get_blob("most_recent_commit"), std::type_identity<frozen_mutation>(), 0)
            : freeze(mutation(s, key));
        most_recent = service::paxos::proposal(row.get_as<utils::UUID>("most_recent_commit_at"),
                std::move(fm));
    }

    return service::paxos::paxos_state(promised, std::move(accepted), std::move(most_recent));
}

future<prepare_response> paxos_state::prepare(storage_proxy& sp, paxos_store& paxos_store, tracing::trace_state_ptr tr_state, schema_ptr schema,
        const query::read_command& cmd, const partition_key& key, utils::UUID ballot,
        bool only_digest, query::digest_algorithm da, clock_type::time_point timeout) {
    co_await utils::get_local_injector().inject("paxos_prepare_timeout", timeout);
    dht::token token = dht::get_token(*schema, key);
    utils::latency_counter lc;
    lc.start();

    auto stats_updater = defer([&sp, schema, lc] () mutable {
        if (auto table = sp.get_db().local().get_tables_metadata().get_table_if_exists(schema->id())) {
            auto& stats = table->get_stats();
            stats.cas_prepare.mark(lc.stop().latency());
        }
    });

    auto guard = co_await get_replica_lock(token, timeout);
    // FIXME: Handle tablet intra-node migration: #16594.
    // The shard can change concurrently, so we cannot rely on locking on this shard.

    // When preparing, we need to use the same time as "now" (that's the time we use to decide if something
    // is expired or not) across nodes, otherwise we may have a window where a Most Recent Decision shows up
    // on some replica and not others during a new proposal (in storage_proxy::begin_and_repair_paxos()), and no
    // amount of re-submit will fix this (because the node on which the commit has expired will have a
    // tombstone that hides any re-submit). See CASSANDRA-12043 for details.
    auto now_in_sec = utils::UUID_gen::unix_timestamp_in_sec(ballot);

    paxos_state state = co_await paxos_store.load_paxos_state(key, schema, gc_clock::time_point(now_in_sec), timeout);
    // If received ballot is newer that the one we already accepted it has to be accepted as well,
    // but we will return the previously accepted proposal so that the new coordinator will use it instead of
    // its own.
    if (ballot.timestamp() > state._promised_ballot.timestamp()) {
        logger.debug("Promising ballot {}", ballot);
        tracing::trace(tr_state, "Promising ballot {}", ballot);
        if (utils::get_local_injector().enter("paxos_error_before_save_promise")) {
            co_await coroutine::return_exception(utils::injected_error("injected_error_before_save_promise"));
        }

        // The all() below throws only if save_paxos_promise fails.
        // If querying the result fails we continue without read round optimization
        auto [data_or_digest] = co_await coroutine::all(
            [&] {
                return paxos_store.save_paxos_promise(*schema, std::ref(key), ballot, timeout);
            },
            [&] () -> future<std::optional<std::variant<foreign_ptr<lw_shared_ptr<query::result>>, query::result_digest>>> {
                try {
                    auto&& [result, hit_rate] = co_await sp.get_db().local().query(schema, cmd,
                            {only_digest ? query::result_request::only_digest : query::result_request::result_and_digest, da},
                            dht::partition_range_vector({dht::partition_range::make_singular({token, key})}), tr_state, timeout);
                    if (only_digest) {
                        co_return *result->digest();
                    } else {
                        co_return make_foreign(std::move(result));
                    }
                } catch(...) {
                    logger.debug("Failed to get data or digest: {}. Ignored.", std::current_exception());
                    co_return std::nullopt;
                }
            }
        );

        if (utils::get_local_injector().enter("paxos_error_after_save_promise")) {
            co_await coroutine::return_exception(utils::injected_error("injected_error_after_save_promise"));
        }

        auto upgrade_if_needed = [schema = std::move(schema), &paxos_store] (std::optional<proposal> p) -> future<std::optional<proposal>> {
            if (!p || p->update.schema_version() == schema->version()) {
                co_return std::move(p);
            }
            // In case current schema is not the same as the schema in the proposal
            // try to look it up first in the local schema_registry cache and upgrade
            // the mutation using schema from the cache.
            //
            // If there's no schema in the cache, then retrieve persisted column mapping
            // for that version and upgrade the mutation with it.
            logger.debug("Stored mutation references outdated schema version. "
                "Trying to upgrade the accepted proposal mutation to the most recent schema version.");
            const column_mapping& cm = co_await paxos_store.get_column_mapping(p->update.column_family_id(), p->update.schema_version());

            co_return std::make_optional(proposal(p->ballot, freeze(p->update.unfreeze_upgrading(schema, cm))));
        };

        auto [u1, u2] = co_await coroutine::all(std::bind(upgrade_if_needed, std::move(state._accepted_proposal)), std::bind(upgrade_if_needed, std::move(state._most_recent_commit)));

        co_return prepare_response(promise(std::move(u1), std::move(u2), std::move(data_or_digest)));
    } else {
        logger.debug("Promise rejected; {} is not sufficiently newer than {}", ballot, state._promised_ballot);
        tracing::trace(tr_state, "Promise rejected; {} is not sufficiently newer than {}", ballot, state._promised_ballot);
        // Return the currently promised ballot (rather than, e.g., the ballot of the last
        // accepted proposal) so the coordinator can make sure it uses a newer ballot next
        // time (#5667).
        co_return std::move(state._promised_ballot);
    }
}

future<bool> paxos_state::accept(storage_proxy& sp, paxos_store& paxos_store, tracing::trace_state_ptr tr_state, schema_ptr schema, dht::token token, const proposal& proposal,
        clock_type::time_point timeout) {
    co_await utils::get_local_injector().inject("paxos_accept_proposal_timeout", timeout);
    utils::latency_counter lc;
    lc.start();

    auto stats_updater = defer([&sp, schema, lc] () mutable {
        if (auto table = sp.get_db().local().get_tables_metadata().get_table_if_exists(schema->id())) {
            auto& stats = table->get_stats();
            stats.cas_accept.mark(lc.stop().latency());
        }
    });

    // FIXME: Handle tablet intra-node migration: #16594.
    // The shard can change concurrently, so we cannot rely on locking on this shard.
    auto guard = co_await get_replica_lock(token, timeout);

    auto now_in_sec = utils::UUID_gen::unix_timestamp_in_sec(proposal.ballot);
    paxos_state state = co_await paxos_store.load_paxos_state(proposal.update.key(), schema, gc_clock::time_point(now_in_sec), timeout);

    // Accept the proposal if we promised to accept it or the proposal is newer than the one we promised.
    // Otherwise the proposal was cutoff by another Paxos proposer and has to be rejected.
    if (proposal.ballot == state._promised_ballot || proposal.ballot.timestamp() > state._promised_ballot.timestamp()) {
        logger.debug("Accepting proposal {}", proposal);
        tracing::trace(tr_state, "Accepting proposal {}", proposal);

        if (utils::get_local_injector().enter("paxos_error_before_save_proposal")) {
            co_await coroutine::return_exception(utils::injected_error("injected_error_before_save_proposal"));
        }

        co_await paxos_store.save_paxos_proposal(*schema, proposal, timeout);

        if (utils::get_local_injector().enter("paxos_error_after_save_proposal")) {
            co_await coroutine::return_exception(utils::injected_error("injected_error_after_save_proposal"));
        }
        co_return true;
    } else {
        logger.debug("Rejecting proposal for {} because in_progress is now {}", proposal, state._promised_ballot);
        tracing::trace(tr_state, "Rejecting proposal for {} because in_progress is now {}", proposal, state._promised_ballot);
        co_return false;
    }
}

future<> paxos_state::learn(storage_proxy& sp, paxos_store& paxos_store, schema_ptr schema, proposal decision, clock_type::time_point timeout,
        tracing::trace_state_ptr tr_state) {
    if (utils::get_local_injector().enter("paxos_error_before_learn")) {
        co_await coroutine::return_exception(utils::injected_error("injected_error_before_learn"));
    }

    utils::latency_counter lc;
    lc.start();

    auto stats_updater = defer([&sp, schema, lc] () mutable {
        if (auto table = sp.get_db().local().get_tables_metadata().get_table_if_exists(schema->id())) {
            auto& stats = table->get_stats();
            stats.cas_learn.mark(lc.stop().latency());
        }
    });

    co_await utils::get_local_injector().inject("paxos_state_learn_timeout", timeout);

    replica::table& cf = sp.get_db().local().find_column_family(schema);
    db_clock::time_point t = cf.get_truncation_time();
    auto truncated_at = std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch());
    // When saving a decision, also delete the last accepted proposal. This is just an
    // optimization to save space.
    // Even though there is no guarantee we will see decisions in the right order,
    // because messages can get delayed, so this decision can be older than our current most
    // recent accepted proposal/committed decision, saving it is always safe due to column timestamps.
    // Since the mutation uses the decision ballot timestamp, if cell timestamp of any current cell
    // is strictly greater than the decision one, saving the decision will not erase it.
    //
    // The table may have been truncated since the proposal was initiated. In that case, we
    // don't want to perform the mutation and potentially resurrect truncated data.
    if (utils::UUID_gen::unix_timestamp(decision.ballot) >= truncated_at) {
        logger.debug("Committing decision {}", decision);
        tracing::trace(tr_state, "Committing decision {}", decision);

        // In case current schema is not the same as the schema in the decision
        // try to look it up first in the local schema_registry cache and upgrade
        // the mutation using schema from the cache.
        //
        // If there's no schema in the cache, then retrieve persisted column mapping
        // for that version and upgrade the mutation with it.
        if (decision.update.schema_version() != schema->version()) {
            on_internal_error(logger, format("schema version in learn does not match current schema"));
        }

        co_await sp.mutate_locally(schema, decision.update, tr_state, db::commitlog::force_sync::yes, timeout);
    } else {
        logger.debug("Not committing decision {} as ballot timestamp predates last truncation time", decision);
        tracing::trace(tr_state, "Not committing decision {} as ballot timestamp predates last truncation time", decision);
    }

    // We don't need to lock the partition key if there is no gap between loading paxos
    // state and saving it, and here we're just blindly updating.
    co_await utils::get_local_injector().inject("paxos_timeout_after_save_decision", timeout);
    co_return co_await paxos_store.save_paxos_decision(*schema, decision, timeout);
}

future<> paxos_state::prune(paxos_store& paxos_store, schema_ptr schema, const partition_key& key, utils::UUID ballot, clock_type::time_point timeout,
        tracing::trace_state_ptr tr_state) {
    logger.debug("Delete paxos state for ballot {}", ballot);
    tracing::trace(tr_state, "Delete paxos state for ballot {}", ballot);
    return paxos_store.delete_paxos_decision(*schema, key, ballot, timeout);
}

int32_t paxos_ttl_sec(const schema& s) {
    // Keep paxos state around for paxos_grace_seconds. If one of the Paxos participants
    // is down for longer than paxos_grace_seconds it is considered to be dead and must rebootstrap.
    // Otherwise its Paxos table state will be repaired by nodetool repair or Paxos repair.
    return std::chrono::duration_cast<std::chrono::seconds>(s.paxos_grace_seconds()).count();
}

paxos_store::paxos_store(gms::feature_service& features, migration_manager& mm,
    sharded<db::system_keyspace>& sys_ks)
    : _features(features)
    , _mm(mm)
    , _sys_ks(sys_ks)
{
}

future<schema_ptr> paxos_store::inject_columns(schema_ptr target) {
    static const auto columns = internal_column_set("$paxos",
        {},
        {
            {"$paxos$promise", timeuuid_type},
            {"$paxos$most_recent_commit", bytes_type}, // serialization format is defined by frozen_mutation idl
            {"$paxos$most_recent_commit_at", timeuuid_type},
            {"$paxos$proposal", bytes_type}, // serialization format is defined by frozen_mutation idl
            {"$paxos$proposal_ballot", timeuuid_type},
        }
    );

    return _mm.inject_internal_columns(target, columns);
}

future<column_mapping> paxos_store::get_column_mapping(table_id table_id, table_schema_version version) {
    return service::get_column_mapping(_sys_ks.local(), table_id, version);
}

future<::shared_ptr<cql3::untyped_result_set>> paxos_store::execute_query(const sstring& query_text, db::timeout_clock::time_point timeout, const std::vector<data_value_or_unset>& parameters) {
    auto& qp = _sys_ks.local().query_processor();
    const auto prepared_stmt_ptr = qp.prepare_internal(query_text);

    const auto now = db::timeout_clock::now();
    const auto d = now < timeout ? timeout - now : db::timeout_clock::duration::zero();
    service::client_state client_state(service::client_state::internal_tag{}, timeout_config{d, d, d, d, d, d, d});
    service::query_state qs{service::client_state::for_internal_calls(), empty_service_permit()};
    const auto qo = qp.make_internal_options(prepared_stmt_ptr, parameters, db::consistency_level::ONE);
    auto msg = co_await qp.do_execute_with_params(qs, prepared_stmt_ptr->statement, qo, std::nullopt);
    co_return ::make_shared<cql3::untyped_result_set>(msg);
}

template <const std::string_view& cql_format, typename... Args>
future<::shared_ptr<cql3::untyped_result_set>> paxos_store::execute_on_partition(
    const schema& s,
    partition_key_view key,
    db::timeout_clock::time_point timeout,
    Args&&... args)
{
    std::vector<data_value_or_unset> params;
    params.reserve(sizeof...(args) + s.partition_key_size());
    (params.push_back(data_value{args}), ...);
    {
        const auto& pk_type = s.partition_key_type();
        auto types_it = pk_type->types().begin();
        for (const auto& c: pk_type->components(key.representation())) {
            params.push_back((*types_it)->deserialize_value(c));
            ++types_it;
        }
    }
    return execute_query(fmt::format(cql_format, s.ks_name(), s.cf_name(), pk_filter_printer{s}),
        timeout, params);
}

bool paxos_store::check_use_tablets(const schema& s) const {
    auto& table = s.table();
    if (!table.uses_tablets()) {
        return false;
    }
    SCYLLA_ASSERT(_features.internal_columns);
    SCYLLA_ASSERT(s.get_column_definition("$paxos$promise") != nullptr);
    return true;
}

future<paxos_state> paxos_store::load_paxos_state(partition_key_view key, schema_ptr s, gc_clock::time_point now,
    db::timeout_clock::time_point timeout)
{
    if (!check_use_tablets(*s)) {
        co_return co_await _sys_ks.local().load_paxos_state(std::move(key), std::move(s), now, timeout);
    }

    // FIXME: we need execute_cql_with_now()
    (void)now;

    static constexpr const std::string_view cql = R"(
        SELECT
            "$paxos$promise",
            "$paxos$most_recent_commit",
            "$paxos$most_recent_commit_at",
            "$paxos$proposal",
            "$paxos$proposal_ballot"
        FROM {}.{}
        WHERE {})";
    const auto result_set = co_await execute_on_partition<cql>(*s, std::move(key), timeout);
    co_return paxos_state::from_row(std::move(key), std::move(s), *result_set);
}

future<> paxos_store::save_paxos_promise(const schema& s, const partition_key& key, const utils::UUID& ballot, db::timeout_clock::time_point timeout) {
    if (!check_use_tablets(s)) {
        return _sys_ks.local().save_paxos_promise(s, key, ballot, timeout);
    }

    static constexpr const std::string_view cql = R"(
        UPDATE {}.{}
        USING TIMESTAMP ? AND TTL ?
        SET "$paxos$promise" = ?
        WHERE {})";
    return execute_on_partition<cql>(s, key.view(), timeout,
        utils::UUID_gen::micros_timestamp(ballot), 
        paxos_ttl_sec(s), 
        ballot)
    .discard_result();
}

future<> paxos_store::save_paxos_proposal(const schema& s, const proposal& proposal, db::timeout_clock::time_point timeout) {
    if (!check_use_tablets(s)) {
        return _sys_ks.local().save_paxos_proposal(s, proposal, timeout);
    }

    static constexpr const std::string_view cql = R"(
        UPDATE {}.{}
        USING TIMESTAMP ? AND TTL ?
        SET
            "$paxos$promise" = ?,
            "$paxos$proposal_ballot" = ?,
            "$paxos$proposal" = ?
        WHERE {})";
    return execute_on_partition<cql>(s, proposal.update.key(), timeout, 
        utils::UUID_gen::micros_timestamp(proposal.ballot),
        paxos_ttl_sec(s),
        proposal.ballot,
        proposal.ballot,
        ser::serialize_to_buffer<bytes>(proposal.update))
    .discard_result();
}

future<> paxos_store::save_paxos_decision(const schema& s, const proposal& decision, db::timeout_clock::time_point timeout) {
    if (!check_use_tablets(s)) {
        return _sys_ks.local().save_paxos_decision(s, decision, timeout);
    }

    static constexpr const std::string_view cql = R"(
        UPDATE {}.{}
        USING TIMESTAMP ? AND TTL ?
        SET
            "$paxos$proposal_ballot" = null,
            "$paxos$proposal" = null,
            "$paxos$most_recent_commit_at" = ?,
            "$paxos$most_recent_commit" = ?
        WHERE {})";
    return execute_on_partition<cql>(s, decision.update.key(), timeout, 
        utils::UUID_gen::micros_timestamp(decision.ballot),
        paxos_ttl_sec(s),
        decision.ballot,
        ser::serialize_to_buffer<bytes>(decision.update))
    .discard_result();
}

future<> paxos_store::delete_paxos_decision(const schema& s, const partition_key& key, const utils::UUID& ballot, db::timeout_clock::time_point timeout) {
    if (!check_use_tablets(s)) {
        return _sys_ks.local().delete_paxos_decision(s, key, ballot, timeout);
    }

    static constexpr const std::string_view cql = R"(
        DELETE "$paxos$most_recent_commit"
        FROM {}.{}
        USING TIMESTAMP ?
        WHERE {})";
    return execute_on_partition<cql>(s, key, timeout,
        utils::UUID_gen::micros_timestamp(ballot))
    .discard_result();
}

} // end of namespace "service::paxos"
