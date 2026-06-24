/*
 * Copyright (C) 2015-present ScyllaDB
 *
 * Modified by ScyllaDB
 */

/*
 * SPDX-License-Identifier: (LicenseRef-ScyllaDB-Source-Available-1.1 and Apache-2.0)
 */

#pragma once

#include "cql3/statements/modification_statement.hh"
#include "cql3/attributes.hh"

#include "data_dictionary/data_dictionary.hh"

namespace cql3 {

namespace statements {

/**
 * An <code>UPDATE</code> statement parsed from a CQL query statement.
 */
class update_statement_impl : public modification_statement_impl {
public:
    update_statement_impl(const statement_type type, schema_ptr schema, uint32_t bound_terms, std::unique_ptr<attributes> attrs);

private:
    void add_update_for_key(mutation& m, const query::clustering_range& range, const update_parameters& params, const json_cache_opt& json_cache) const override;

    virtual void execute_operations_for_key(mutation& m, const clustering_key_prefix& prefix, const update_parameters& params, const json_cache_opt& json_cache) const;
};

/*
 * Update statement specification that has specifically one bound name - a JSON string.
 * Overridden add_update_for_key uses this parsed JSON to look up values for columns.
 */
class insert_prepared_json_statement_impl : public update_statement_impl {
    expr::expression _value;
    bool _default_unset;
public:
    insert_prepared_json_statement_impl(schema_ptr schema, uint32_t bound_terms, std::unique_ptr<attributes> attrs, expr::expression v, bool default_unset);

private:
    void execute_operations_for_key(mutation& m, const clustering_key_prefix& prefix, const update_parameters& params, const json_cache_opt& json_cache) const override;

    dht::partition_range_vector build_partition_keys(const query_options& options, const json_cache_opt& json_cache) const override;

    query::clustering_row_ranges create_clustering_ranges(const query_options& options, const json_cache_opt& json_cache) const override;

    json_cache_opt maybe_prepare_json_cache(const query_options& options) const override;

    void execute_set_value(mutation& m, const clustering_key_prefix& prefix, const update_parameters&
        params, const column_definition& column, const bytes_opt& value) const;
};

}

}
