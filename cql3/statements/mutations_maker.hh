/*
 * Copyright (C) 2015-present ScyllaDB
 *
 * Modified by ScyllaDB
 */

/*
 * SPDX-License-Identifier: (LicenseRef-ScyllaDB-Source-Available-1.1 and Apache-2.0)
 */

#pragma once

#include "seastarx.hh"
#include "schema/schema.hh"

#include <seastar/core/shared_ptr.hh>

#include <memory>
#include <vector>

namespace cql3 {

class operation;

namespace restrictions {
class statement_restrictions;
}

namespace statements {

// Prepare-time state required to turn a set of query_options into mutations,
// independent of how (or whether) those mutations are then executed.
//
// This is the piece of a modification statement that query_processor's
// get_mutations_internal() actually needs: the parsed primary-key/clustering
// restrictions, the per-column update operations, and the prefetch bookkeeping
// (which columns must be read before the write, and whether a read is required
// at all).
//
// It currently lives as a member of modification_statement and will gradually
// absorb the rest of the mutation-production state and logic, so that the
// producer can be reused by the strongly-consistent and batch execution paths
// without dragging the execution-only machinery along. Fields are public for
// now; encapsulation will follow once the responsibilities have settled.
struct mutations_maker {
    std::vector<std::unique_ptr<operation>> _column_operations;
    ::shared_ptr<const restrictions::statement_restrictions> _restrictions;
    // If we have operation on list entries, such as adding or
    // removing an entry, the modification statement must prefetch
    // the old values of the list to create an idempotent mutation.
    // If the statement has conditions, conditional columns must
    // also be prefetched, to evaluate conditions. If the
    // statement has IF EXISTS/IF NOT EXISTS, we prefetch all
    // columns, to match Cassandra behaviour.
    // This bitset contains a mask of ordinal_id identifiers
    // of the required columns.
    column_set _columns_to_read;
    // True if any of update operations requires a prefetch.
    // Pre-computed during statement prepare.
    bool _requires_read = false;

    explicit mutations_maker(column_count_type all_columns_count);
    ~mutations_maker();
};

}

}
