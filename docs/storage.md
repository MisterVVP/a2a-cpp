# Storage backends

The SDK storage layer is pluggable. `TaskLifecycleService` uses the `TaskStore` interface and `PushNotificationService` uses the `PushNotificationStore` interface, so applications can provide in-memory stores for development or PostgreSQL-backed stores for local production-style testing.

## In-memory stores

Use `InMemoryStoreFactory` for examples, tests, and single-process development:

```cpp
a2a::server::stores::InMemoryStoreFactory factory;
auto stores = factory.CreateStoreBundle();
a2a::examples::ExampleExecutor executor({
    .task_store = stores.value().task_store.get(),
    .push_store = stores.value().push_store.get(),
});
```

In-memory stores do not persist data, do not coordinate across processes, and should not be used as durable production storage.

## PostgreSQL build flag

PostgreSQL support is optional and is disabled by default. A default build does not require PostgreSQL, `libpq`, or `libpqxx`:

```bash
cmake -S . -B build
cmake --build build
```

Enable the PostgreSQL store target with:

```bash
cmake -S . -B build-postgres -DA2A_ENABLE_POSTGRES_STORE=ON -DA2A_ENABLE_TESTING=ON
cmake --build build-postgres
```

When enabled, CMake builds `a2a::store_postgres`. Applications that instantiate PostgreSQL stores should link this target in addition to the normal server target.

## Local Docker PostgreSQL

Start a local PostgreSQL 16 database:

```bash
docker compose -f dev/local/docker-compose.postgres.yml up -d
```

Equivalent helper script:

```bash
./dev/local/postgres_up.sh
```

Run a PostgreSQL-enabled build and test suite:

```bash
cmake -S . -B build-postgres -DA2A_ENABLE_POSTGRES_STORE=ON -DA2A_ENABLE_TESTING=ON
cmake --build build-postgres
A2A_TEST_POSTGRES_DSN="postgresql://a2a:a2a@127.0.0.1:5432/a2a" ctest --test-dir build-postgres --output-on-failure
```

Stop the database when done:

```bash
docker compose -f dev/local/docker-compose.postgres.yml down
```

Equivalent helper script:

```bash
./dev/local/postgres_down.sh
```

See `dev/local/README.md` for all local SDK development helper commands.

## Example executor injection

`ExampleExecutor` owns default in-memory stores when no options are supplied, preserving the existing examples. External callers can inject any implementation of the store interfaces:

```cpp
a2a::server::stores::PostgresStoreFactory factory({
    .connection_string = "postgresql://a2a:a2a@127.0.0.1:5432/a2a",
    .schema = "public",
    .auto_create_schema = true,
    .connection_pool_size = 8,
});
auto stores = factory.CreateStoreBundle();

a2a::examples::ExampleExecutor executor({
    .task_store = stores.value().task_store.get(),
    .push_store = stores.value().push_store.get(),
});
```

`connection_pool_size` controls the number of synchronous libpq connections
available to PostgreSQL store operations. It defaults to `4` for backward
compatibility and must be greater than zero. Size it for the expected number of
concurrent database operations while staying within PostgreSQL's connection
limit (including connections used by other application instances and tools).
Task and push-notification stores returned by `CreateStoreBundle()` share this
single configured pool; separately created stores each own a separate pool.
Storage matching uses libpq's effective logical connection options rather than
the active resolved server IP, so equivalent URI/keyword DSNs and stable
DNS/multi-host configurations remain comparable across reconnects. Exact
logical storage coordinates confirm local authority; a different database or
schema confirms external authority. Other PostgreSQL mismatches, including
`host`, `hostaddr`, port, and `target_session_attrs`, are treated as uncertain
because they can represent either aliases/failover or genuinely different
servers. Task-aware creates reject uncertain authority before writing
provenance instead of treating an identity mismatch as external ownership.
The effective PostgreSQL role is tracked separately: role differences disable
same-execution read shortcuts but do not by themselves make storage external.

## Externally managed PostgreSQL schemas

When `auto_create_schema=false`, the push-notification store validates the
`task-aware-push-config-v2` migration during construction. Startup fails with an
actionable error instead of allowing the first request to fail against an
outdated or insecure schema. Validation covers the provenance column type,
nullability and `FALSE` default; removal of the legacy task foreign key; both
SECURITY DEFINER helpers and their owners' required privileges; absence of
`PUBLIC EXECUTE`; the cleanup implementation/version; and exact cleanup-trigger
wiring without a `WHEN` clause.

Apply the following migration before upgrading. This example uses the `public`
schema and an SDK database role named `a2a_sdk`; replace both names for your
deployment. Grant the lock helper only to SDK roles authorized to create push
notification configurations. Task-table `SELECT` and lock-helper `EXECUTE` are
required only when that role uses the local PostgreSQL task-aware create path;
push-only roles paired with an external authoritative `TaskStore` do not need
either privilege. PostgreSQL row-level security on the task table is allowed
for external-authority/push-only use, but the local task-aware create path
rejects it explicitly because a SECURITY DEFINER lock must not bypass caller
row policies.

```sql
BEGIN;

ALTER TABLE public.a2a_push_notification_configs
  ADD COLUMN IF NOT EXISTS local_postgres_task BOOLEAN NOT NULL DEFAULT FALSE;

ALTER TABLE public.a2a_push_notification_configs
  DROP CONSTRAINT IF EXISTS a2a_push_configs_task_fk;

CREATE OR REPLACE FUNCTION public.a2a_lock_task_for_push_config(requested_task_id TEXT)
RETURNS BOOLEAN
LANGUAGE plpgsql
VOLATILE
SECURITY DEFINER
SET search_path = pg_catalog
AS $a2a$
DECLARE
  caller_role NAME;
BEGIN
  caller_role := NULLIF(pg_catalog.current_setting('role', true), 'none');
  IF caller_role IS NULL THEN
    caller_role := session_user;
  END IF;
  IF EXISTS (
    SELECT 1 FROM pg_catalog.pg_class
    WHERE oid = pg_catalog.to_regclass('public.a2a_tasks') AND relrowsecurity
  ) THEN
    RAISE EXCEPTION USING ERRCODE = '0A000', MESSAGE =
      'PostgreSQL task-aware push configuration does not support row-level security on a2a_tasks';
  END IF;
  IF NOT pg_catalog.has_table_privilege(caller_role, 'public.a2a_tasks', 'SELECT') THEN
    RAISE EXCEPTION USING ERRCODE = '42501', MESSAGE =
      'PostgreSQL push store role requires SELECT on a2a_tasks for task-aware creation';
  END IF;
  PERFORM 1
  FROM public.a2a_tasks
  WHERE id = requested_task_id
  FOR KEY SHARE;
  RETURN FOUND;
END
$a2a$;

REVOKE ALL ON FUNCTION public.a2a_lock_task_for_push_config(TEXT) FROM PUBLIC;
GRANT SELECT ON public.a2a_tasks TO a2a_sdk;
GRANT EXECUTE ON FUNCTION public.a2a_lock_task_for_push_config(TEXT) TO a2a_sdk;

CREATE OR REPLACE FUNCTION public.a2a_delete_task_push_configs()
RETURNS TRIGGER
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog
AS $a2a$
BEGIN
  DELETE FROM public.a2a_push_notification_configs
  WHERE task_id = OLD.id AND local_postgres_task;
  RETURN OLD;
END
$a2a$;

REVOKE ALL ON FUNCTION public.a2a_delete_task_push_configs() FROM PUBLIC;

DROP TRIGGER IF EXISTS a2a_delete_task_push_configs_trigger ON public.a2a_tasks;
CREATE TRIGGER a2a_delete_task_push_configs_trigger
AFTER DELETE ON public.a2a_tasks
FOR EACH ROW
EXECUTE FUNCTION public.a2a_delete_task_push_configs();

COMMENT ON FUNCTION public.a2a_delete_task_push_configs()
  IS 'task-aware-push-config-v2';

COMMENT ON FUNCTION public.a2a_lock_task_for_push_config(TEXT)
  IS 'task-aware-push-config-v2';

COMMIT;
```

The migration markers are written last inside the transaction, so a partial
migration is never accepted. Existing push configurations are marked as
externally owned by the new column's `FALSE` default. The owner of
`a2a_lock_task_for_push_config(TEXT)` must have schema `USAGE` plus `SELECT` and
`UPDATE` on `a2a_tasks`; the `UPDATE` privilege is needed by PostgreSQL for
`FOR KEY SHARE`. The owner of `a2a_delete_task_push_configs()` must have schema
`USAGE` plus `SELECT` and `DELETE` on the push-config table. These are SECURITY
DEFINER owner requirements, not privileges that every push-store role must receive.

## Sensitive push notification data

Push notification configs can include delivery URLs, tokens, and authentication metadata. Treat stored `TaskPushNotificationConfig` payloads as sensitive operational data. This SDK stores protobuf payloads in PostgreSQL but does not implement encryption at rest; use database-level encryption, access control, backups, and audit logging appropriate for your environment.
