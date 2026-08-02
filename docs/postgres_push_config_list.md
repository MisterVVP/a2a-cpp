# PostgreSQL push-config list query

The PostgreSQL store executes a successful push-config list as one database
command. Before this change the service issued `task_get`, followed by separate
`push_config_list_count` and `push_config_list_select` commands. The combined
query is reported as one `push_config_list_select` call; the expected per-operation
counts are therefore `task_get = 0`, `push_config_list_count = 0`, and
`push_config_list_select = 1`.

The statement uses a materialized task CTE, a materialized config count, and a
lateral ordered page. The outer left join emits one tagged row when the task
exists but the requested page contains no configs. No row means the task is
missing, a row with a null config means an empty list or valid empty page, and a
non-null config contains a selected protobuf. A token greater than the count is
still rejected; a token equal to the count is a valid empty page. A zero page
size passes a SQL `NULL` limit (unbounded), while positive sizes are bounded.

Count and page selection share one PostgreSQL statement snapshot, so a
concurrent insert or delete cannot make the count disagree with the selected
rows inside an operation. Offset tokens retain their existing behaviour between
operations. Selection remains stable by `created_sequence ASC`. PostgreSQL can
use the task primary key for the task CTE, the task-id push-config index for the
count, and `(task_id, created_sequence ASC)` for the ordered page.

PostgreSQL conformance validation covers existing tasks with zero configs,
bounded and unbounded pages, stable creation order, a token equal to the total
count, an out-of-range token, and a missing task. Every case asserts one
`push_config_list_select` call and no `task_get` or separate
`push_config_list_count` call. The performance runner reviews the complete
combined CTE/lateral plan with `EXPLAIN (ANALYZE, BUFFERS)` and requires the task
primary key, push-config task, and push-config creation-order indexes.

The baseline measurements from PR #184 used three commands per successful
operation and reported operation p95 `12.10 ms`, task-get p95 `5.01 ms`, count
p95 `4.79 ms`, select p95 `4.87 ms`, and effectively zero connection-acquire
p95 at pool size and concurrency 64. The proposed statement's observed call
count is covered by the PostgreSQL store test. Latency comparisons require a
configured PostgreSQL performance environment and should use pool size 64,
concurrency 1, 4, 16, and 64, fixed fan-out/page-size fixtures, repeated runs,
and zero operation errors.

`PushConfig_ListManyConfigs` remains the focused performance scenario because
it exercises the optimized unbounded list over a fixed fan-out and isolates the
database command reduction. Bounded-page variants use the same combined query
and indexes and are covered as correctness and query-plan tests; adding them to
the repeated performance matrix is deferred to a follow-up to avoid multiplying
the matrix without evidence that `LIMIT` changes the performance conclusion.
