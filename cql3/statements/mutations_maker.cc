/*
 * Copyright (C) 2015-present ScyllaDB
 *
 * Modified by ScyllaDB
 */

/*
 * SPDX-License-Identifier: (LicenseRef-ScyllaDB-Source-Available-1.1 and Apache-2.0)
 */

#include "cql3/statements/mutations_maker.hh"
#include "cql3/operation.hh"

namespace cql3 {

namespace statements {

mutations_maker::mutations_maker(column_count_type all_columns_count)
    : _columns_to_read(all_columns_count)
{ }

mutations_maker::~mutations_maker() = default;

}

}
