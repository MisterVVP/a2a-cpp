# PostgreSQL push-config list query

The PostgreSQL store executes a direct task-aware push-config list as one
database command. Service-level listing first validates the task against the
service's configured authoritative `TaskStore`, then calls the push store's
existing-task path. This keeps independently configured or mixed stores correct
while reducing the previous three-command service operation to two commands:
`task_get = 1`, `push_config_list_count = 0`, and
`push_config_list_select = 1`. Calls that already hold a validated task, such as
push delivery, use only `push_config_list_select = 1`.

The statement uses a materialized task CTE, a materialized config count, and a
lateral ordered page. The outer left join emits one tagged row when the task
exists but the requested page contains no configs. No row means the task is
missing, a row with a null config means an empty list or valid empty page, and a
non-null config contains a selected protobuf. A token greater than the count is
still rejected; the lateral page scan is gated on the token being in range. A
token equal to the count is a valid empty page. Tokens outside PostgreSQL's
signed `bigint` range are rejected before a database command is issued. A zero
page size passes a SQL `NULL` limit (unbounded), while positive sizes are bounded.

Count and page selection share one PostgreSQL statement snapshot, so a
concurrent insert or delete cannot make the count disagree with the selected
rows inside an operation. Offset tokens retain their existing behaviour between
operations. Selection remains stable by `created_sequence ASC`. During the issue
#206 ordering-index experiment, both push secondary indexes are absent. PostgreSQL
may use the push table primary key `(task_id, config_id)` for `task_id` filtering
and then explicitly sort the selected rows by `created_sequence ASC`.

PostgreSQL conformance validation covers existing tasks with zero configs,
bounded and unbounded pages, stable creation order, a token equal to the total
count, an out-of-range token, and a missing task. Every case asserts one
`push_config_list_select` call and no `task_get` or separate
`push_config_list_count` call. It also covers rejection before query execution
for tokens beyond PostgreSQL's `bigint` range. The performance runner reviews
the complete combined CTE/lateral plan, including the invalid-offset gate, with
`EXPLAIN (ANALYZE, BUFFERS)`. For this experiment it rejects use of the removed
ordering index and requires the plan to expose the explicit `created_sequence`
sort. The captured plan remains the source of truth for whether PostgreSQL chose
the push primary key or a sequential scan, as well as sort method/memory and
buffer activity.

The baseline measurements from PR #184 used three commands per successful
operation and reported operation p95 `12.10 ms`, task-get p95 `5.01 ms`, count
p95 `4.79 ms`, select p95 `4.87 ms`, and effectively zero connection-acquire
p95 at pool size and concurrency 64. The proposed statement's observed call
count is covered by the PostgreSQL store test. Latency comparisons require a
configured PostgreSQL performance environment. The `postgres-tail` profile
runs five repetitions at concurrency 4, 16, and 64 across pool sizes 4, 16,
and 64. The separate `postgres-tail-c1` profile runs five repetitions of
`PushConfig_ListManyConfigs` at concurrency 1 and pool size 64. This retains
the focused single-client regression signal without repeating every scenario
and pool-size coordinate serially. Both profiles require zero operation errors.

`PushConfig_ListManyConfigs` remains the focused performance scenario because
it exercises the optimized unbounded list and isolates the database command
reduction. Its fixture fan-out defaults to 8 and can be overridden with
`--push-config-fanout` without changing the production path. For the issue #206
ordering-index experiment, run the existing five-repetition c1/pool-64 profile
at each required fan-out:

```bash
for fanout in 8 100 1000 10000; do
  A2A_TEST_POSTGRES_DSN=postgresql://a2a:a2a@127.0.0.1:5432/a2a \
  ./scripts/run_performance_tests.sh --profile postgres-tail-c1 \
    --push-config-fanout "${fanout}" \
    --report-dir "perf-artifacts/postgres-list-${fanout}"
done
```

Each report records list throughput/latency and the representative
`EXPLAIN (ANALYZE, BUFFERS)` plan for the same configured fan-out. Bounded-page
variants use the same combined query and are covered as correctness tests.
