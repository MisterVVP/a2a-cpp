# SDK idempotency and task-history deduplication specification

## Duplicate definition
A task-history append is considered a duplicate when it matches an existing history entry according to the selected `HistoryAppendPolicy`:

- `kNoDedup`: no duplicate detection; every request is appended.
- `kDedupByMessageId`: duplicate only when both `message_id` and the message fingerprint match.
- `kDedupByIdOrFingerprint`: with a `message_id`, same behavior as `kDedupByMessageId`; without a `message_id`, duplicate is determined by fingerprint only.

Fingerprint equality is evaluated on `task_id`, `context_id`, `role`, `parts_size`, and serialized `parts` content.

## Ignored vs appended behavior
- **Ignored (dropped)**: only when duplicate conditions above are met for the active policy.
- **Appended**:
  - always for `kNoDedup`;
  - for `kDedupByMessageId` when `message_id` is missing, or when payload differs;
  - for `kDedupByIdOrFingerprint` when neither id+fingerprint nor fingerprint-only duplicate match exists.

History order is strict append order for accepted entries. Retries that are accepted are appended at the end (no backfilling/reordering).

## Missing `message_id`
For idempotent retry behavior across transports, clients should set `message_id` consistently per logical message.

When `message_id` is missing:
- `kDedupByMessageId` cannot dedupe and the request is appended.
- `kDedupByIdOrFingerprint` attempts fingerprint-based dedupe.

## Structured telemetry for dedupe drops
`InMemoryTaskStore` maintains counters via `GetHistoryTelemetrySnapshot()`:
- `dedupe_dropped_total`
- `dedupe_dropped_by_message_id_and_fingerprint`
- `dedupe_dropped_by_fingerprint_without_message_id`

Operators can also provide `InMemoryTaskStore::HistoryTelemetrySink` to consume structured `HistoryDedupeEvent` records (`task_id`, `message_id`, `policy`, `reason`) for retry debugging.
