# ListTasks cursor pagination

This contract targets v0.6.0 development. Released v0.5.2 retains numeric
continuation tokens and creation-order listing; see the [release notes](../releases.md).

The SDK stores list tasks by storage last-update time, descending, then task ID
in descending byte order. The time is independent of `TaskStatus.timestamp`.
Every successful create, unconditional/conditional replacement (including metadata,
status, history or artifacts), cancellation, and history append advances it.
Rejected writes, revision conflicts, rejected cancellations and deduplicated history
appends do not change it. Each task's time advances by at least one microsecond,
even if the wall clock moves backward. Equal times across tasks use the ID tie-breaker.

Start with an empty `page_token`. Copy the returned `next_page_token` unchanged
into the next request; stop when it is empty. Tokens are opaque, versioned v1
boundaries, portable between JSON-RPC, HTTP+JSON and gRPC for the same store.
They are neither credentials nor encrypted data. They never require resolving the
boundary task or reveal whether that task currently exists or is accessible.
Only a returned, authorized task supplies a response token.

## Compatibility

Numeric offset tokens (including `0`) are rejected as invalid arguments. Restart
an old pagination sequence with an empty token. Ordering intentionally changes
from creation order. The public client loop in
[Get Task and List Tasks](../client/get-task.md) already follows returned
tokens and requires no arithmetic. Do not manufacture, increment, decode, persist
as an application identifier, or depend on a token's representation. Unsupported
versions, invalid encodings, empty IDs and out-of-range times use the same generic
validation error regardless of the store contents or caller scope.

Keep the same caller and filters for a traversal. Changing them applies the
existing boundary to the new visible result set; it does not grant access to the
previous caller's tasks. An empty page is valid after updates or permission changes.
It has an empty continuation token, even when the current total is nonzero.

## Consistency and permissions

Pagination is a live traversal, not a snapshot spanning requests. Inserts and
updates ahead of the boundary are seen on a fresh traversal. A task updated or
canceled between pages can move ahead of the boundary and be omitted from the
rest of that traversal; already traversed tasks do not move backward and repeat.
Equal-time inserts after the ID boundary can appear in subsequent pages.

An in-memory store call holds one shared lock. A PostgreSQL store call reads its
count and page in one repeatable-read, read-only transaction. The authorization
dispatcher narrows store queries with `ScopeTaskList`, checks `CanAccessTask`
before counting and page selection, then re-reads and rechecks selected tasks
before projecting them. A revocation during that final read returns the same
not-found error as an inaccessible task. Applications own policy synchronization.
Authorization scans use bounded batches and can observe changes between batches;
their counts represent authorized observations during that scan, not a global
snapshot of a changing application policy. Totals exclude inaccessible tasks and
are independent of the cursor boundary.

## Costs and custom stores

In-memory writes maintain an ordered index in O(log n). An unfiltered page seeks
in O(log n) and visits only page-size plus one entries. Filtered exact counts
still inspect all stored tasks. PostgreSQL uses `(updated_at DESC, id COLLATE "C"
DESC)` and context-prefixed ordering indexes; SQL applies filters before its cursor
predicate and bounded LIMIT. Both keys descend so a composite row comparison can
seek directly through equal timestamps. Exact counts still scan matching rows/index entries.
These indexes add write and storage cost.

Application-defined authorization can require inspecting every matching task.
The dispatcher retains one batch (at most 100 tasks) and one page of IDs, disables
redundant store counts, and loads full response projections only for selected IDs.
Policies may request lightweight projections only when those fields suffice for
authorization. Complex policies may therefore decode all matching task blobs.

Custom `TaskStore` implementations used with dispatcher security must return
`ListTasksResponse.task_cursors` aligned with `tasks`, containing storage update
times and IDs, and honor the same order/continuation contract. Missing boundaries
fail closed. The boundaries and `ListTasksRequest.include_total_size` flag are
internal store/dispatcher fields and are never transmitted on the wire. Set
`include_total_size=false` to skip redundant counts during bounded internal scans.

SDK-managed PostgreSQL schemas install the default and ordering indexes on startup.
Externally managed schemas must apply the equivalent migration before deploying:

```sql
ALTER TABLE public.a2a_tasks ALTER COLUMN updated_at SET DEFAULT clock_timestamp();
CREATE INDEX IF NOT EXISTS idx_a2a_tasks_updated
  ON public.a2a_tasks (updated_at DESC, id COLLATE "C" DESC);
CREATE INDEX IF NOT EXISTS idx_a2a_tasks_context_updated
  ON public.a2a_tasks (context_id, updated_at DESC, id COLLATE "C" DESC);
```

Existing rows retain their persisted `updated_at`; no creation timestamps are
invented. All writers must update `updated_at` monotonically with task mutations.
Push-notification configuration pagination is independent and retains its contract.
