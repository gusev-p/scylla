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
#include "data_dictionary/data_dictionary.hh"

namespace cql3 {

class attributes;

namespace statements {

/**
* A <code>DELETE</code> parsed from a CQL query statement.
*/
class delete_statement_impl : public modification_statement_impl {
public:
    delete_statement_impl(schema_ptr s, uint32_t bound_terms, std::unique_ptr<attributes> attrs);

    void add_update_for_key(mutation& m, const query::clustering_range& range, const update_parameters& params, const json_cache_opt& json_cache) const override;
};

}

}
