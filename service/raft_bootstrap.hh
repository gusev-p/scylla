/*
 *
 * Modified by ScyllaDB
 * Copyright (C) 2015-present ScyllaDB
 *
 */

/*
 * SPDX-License-Identifier: (AGPL-3.0-or-later and Apache-2.0)
 */

#pragma once

#include <stdint.h>
#include <iostream>

namespace service {
// Raft leader uses this command to drive bootstrap process on other nodes
struct raft_bootstrap_cmd {
      enum class command: uint8_t {
          barrier,
          stream_ranges,
          fence_old_reads
      };
      command cmd;
};

// returned as a result of raft_bootstrap_cmd
struct raft_bootstrap_cmd_result {
    enum class command_status: uint8_t {
        fail,
        success
    };
    command_status status = command_status::fail;
};

inline std::ostream& operator<<(std::ostream& os, const raft_bootstrap_cmd::command& cmd) {
    switch (cmd) {
        case raft_bootstrap_cmd::command::barrier:
            os << "barrier";
            break;
        case raft_bootstrap_cmd::command::stream_ranges:
            os << "stream_ranges";
            break;
        case raft_bootstrap_cmd::command::fence_old_reads:
            os << "fence_old_reads";
            break;
    }
    return os;
}

struct fencing_token {
    int64_t topology_version;
};

inline std::ostream& operator<<(std::ostream& os, const fencing_token& fencing_token) {
    return os << "fencing_token(" << fencing_token.topology_version << ")";
}

// todo rename to raft_topology.hh?

}
