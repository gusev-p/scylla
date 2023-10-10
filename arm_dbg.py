import subprocess
import yaml

stderr_file = open("/home/gusev-p/arm_dbg/stderr", "wb")
stdout_file = open("/home/gusev-p/arm_dbg/stdout", "wb")

config_file_path = '/home/gusev-p/arm_dbg/scylla.yaml'
scylla_host_addr = '127.251.251.42'
scylla_conf = {
    'cluster_name': 'arm_dbg',
    'workdir': '/home/gusev-p/arm_dbg/work_dir',
    'listen_address': scylla_host_addr,
    'rpc_address': scylla_host_addr,
    'api_address': scylla_host_addr,
    'prometheus_address': scylla_host_addr,
    'alternator_address': scylla_host_addr,
    'seed_provider': [{
        'class_name': 'org.apache.cassandra.locator.SimpleSeedProvider',
        'parameters': [{
            'seeds': '{}'.format(','.join([]))
        }]
    }],

    'developer_mode': True,

    # Allow testing experimental features. Following issue #9467, we need
    # to add here specific experimental features as they are introduced.
    'enable_user_defined_functions': True,
    'experimental_features': ['udf',
                              'alternator-streams',
                              'consistent-topology-changes',
                              'broadcast-tables',
                              'keyspace-storage-options',
                              'tablets'],

    'consistent_cluster_management': True,

    'skip_wait_for_gossip_to_settle': 0,
    'ring_delay_ms': 0,
    'num_tokens': 16,
    'flush_schema_tables_after_modification': False,
    'auto_snapshot': False,

    # Significantly increase default timeouts to allow running tests
    # on a very slow setup (but without network losses). Note that these
    # are server-side timeouts: The client should also avoid timing out
    # its own requests - for this reason we increase the CQL driver's
    # client-side timeout in conftest.py.

    'range_request_timeout_in_ms': 300000,
    'read_request_timeout_in_ms': 300000,
    'counter_write_request_timeout_in_ms': 300000,
    'cas_contention_timeout_in_ms': 300000,
    'truncate_request_timeout_in_ms': 300000,
    'write_request_timeout_in_ms': 300000,
    'request_timeout_in_ms': 300000,

    'strict_allow_filtering': True,
    'strict_is_not_null_in_views': True,

    'permissions_update_interval_in_ms': 100,
    'permissions_validity_in_ms': 100,

    'reader_concurrency_semaphore_serialize_limit_multiplier': 0,
    'reader_concurrency_semaphore_kill_limit_multiplier': 0,

    'force_schema_commit_log': True,
}
with open(config_file_path, 'w') as config_file:
    yaml.dump(scylla_conf, config_file)

args = [
    '/home/gusev-p/scylladb/build/debug/scylla',
    '--smp', '50',
    '--options-file', config_file_path,
    '-m', '50G',
    '--collectd', '0',
    '--overprovisioned',
    '--max-networking-io-control-blocks', '1000',
    '--unsafe-bypass-fsync', '1',
    '--kernel-page-cache', '1',
    '--commitlog-use-o-dsync', '0',
    '--abort-on-lsa-bad-alloc', '1',
    '--abort-on-seastar-bad-alloc',
    '--abort-on-internal-error', '1',
    '--abort-on-ebadf', '1'
]

completed_scylla = subprocess.run(args, stdout=stdout_file, stderr=stderr_file)
stderr_file.flush()
stdout_file.flush()
print(f'scylla finished, returncode {completed_scylla.returncode}')
