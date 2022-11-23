/*
 * Copyright 2022-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

namespace service {
  struct raft_bootstrap_cmd {
      enum class command: uint8_t {
          barrier,
          stream_ranges,
          fence_old_reads
      };
      service::raft_bootstrap_cmd::command cmd;
  };

  struct raft_bootstrap_cmd_result {
      enum class command_status: uint8_t {
          fail,
          success
      };
      service::raft_bootstrap_cmd_result::command_status status;
  };

  verb raft_bootstrap_cmd (service::raft_bootstrap_cmd) -> service::raft_bootstrap_cmd_result;
}
