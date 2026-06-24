/*
 * Copyright (C) 2025-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: LicenseRef-ScyllaDB-Source-Available-1.1
 */

#pragma once

#include "cql3/cql_statement.hh"
#include "cql3/expr/expression.hh"
#include "cql3/statements/modification_statement.hh"

namespace cql3::statements::strong_consistency {

class modification_statement : public cql_statement_opt_metadata {
    using result_message = cql_transport::messages::result_message;
    using modification_statement_impl = cql3::statements::modification_statement_impl;

    shared_ptr<modification_statement_impl> _impl;
public:
    modification_statement(shared_ptr<modification_statement_impl> impl);

    future<shared_ptr<result_message>> execute(query_processor& qp, service::query_state& state,
        const query_options& options, std::optional<service::group0_guard> guard) const override;

    future<shared_ptr<result_message>> execute_without_checking_exception_message(query_processor& qp,
        service::query_state& qs, const query_options& options,
        std::optional<service::group0_guard> guard) const override;

    future<> check_access(query_processor& qp, const service::client_state& state) const override;

    uint32_t get_bound_terms() const override;

    bool depends_on(std::string_view ks_name, std::optional<std::string_view> cf_name) const override;
};

}
