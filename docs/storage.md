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
Storage matching uses libpq's effective logical connection options (`host`,
`hostaddr`, port, database, and `target_session_attrs`) plus the configured
schema. Passwords and raw connection strings are not part of storage identity.
Exact coordinate equality confirms local authority; a different database or
schema confirms external authority. Other endpoint differences are uncertain
because aliases, DNS, `hostaddr`, port changes, and multi-host failover can make
textual differences insufficient evidence of external ownership. Equivalent URI
and keyword DSNs therefore take the local path only when libpq reports the same
effective logical options.

The effective PostgreSQL role is tracked separately as part of the execution
identity. Role differences do not make storage external and do not disable the
one-command local create path. They do disable the same-execution list shortcut,
which requires both equal storage coordinates and the same effective role. Each
connection pool caches the first established execution identity and verifies
other initial and reopened connections against it. Applications must not leave a
pooled connection under an out-of-band `SET ROLE`.

### Task-aware PostgreSQL behavior

`PostgresPushNotificationStore` implements the optional task-aware store
capability used by `PushNotificationService` for create and list operations. The
final PostgreSQL paths are:

- **Same storage, create/update:** one push-store pool acquisition and one
  `PQexecParams` command. The statement invokes the schema-qualified
  `SECURITY DEFINER` task-lock helper, takes `FOR KEY SHARE` on the task, and
  performs `INSERT ... ON CONFLICT DO UPDATE ... RETURNING 1` in the same
  statement. There is no preliminary `task_get`, explicit multi-command
  transaction, revalidation, or compensating cleanup. A missing task returns
  `TaskNotFound`; a concurrent delete cannot leave a locally owned orphan.
- **External authority, create/update:** the supplied `TaskStore` remains
  authoritative. The service performs its task lookup first and then writes an
  externally owned push row (`local_postgres_task=FALSE`). A different database
  or schema proves external authority. An uncertain PostgreSQL identity is never
  treated as external and is rejected before the push write. Direct
  `PushNotificationStore::CreateOrUpdate` calls have no authoritative
  `TaskStore`, so PostgreSQL keeps them on this external-provenance path.
- **Get:** `GetConfig` remains push-store-only. PostgreSQL uses one statement and
  one push-store acquisition to distinguish a missing config from an absent
  push-config collection without consulting the authoritative `TaskStore`.
- **List:** matching execution identities use one combined PostgreSQL statement
  for task existence, count, and page rows. When execution identities differ,
  the authoritative task lookup runs first and the push store then executes one
  combined count/page statement.

For the normal `PostgresStoreFactory::CreateStoreBundle()` layout, create, get,
and list therefore use one PostgreSQL command and one pool acquisition per
successful public operation. `PushConfig_CreateMany` with fan-out eight performs
eight independent create commands, not sixteen; the task existence check is part
of each atomic create statement. Split-role stores that resolve to the same
storage coordinates use the same one-command create path. Split-role list calls
use the task-first fallback because their execution identities differ.

When `auto_create_schema=true`, schema initialization creates or upgrades the
task/push tables, provenance column, helper functions, cleanup trigger, sequences,
and indexes, and removes the legacy push-to-task foreign key. The migration
objects are installed transactionally and the `task-aware-push-config-v2`
markers are written last.

## Externally managed PostgreSQL schemas

When `auto_create_schema=false`, the push-notification store validates the
`task-aware-push-config-v2` migration during construction. Startup fails with an
actionable error instead of allowing the first request to fail against an
outdated or insecure schema. Validation covers the provenance column type,
nullability and `FALSE` default; absence of a push-to-task foreign key; both
`SECURITY DEFINER` helpers and their owners' required privileges; absence of
`PUBLIC EXECUTE`; the cleanup implementation/version; and exact `AFTER DELETE`
trigger wiring without a `WHEN` clause.

Apply the following migration before upgrading. This example uses the `public`
schema and an SDK database role named `a2a_sdk`; replace both names for your
deployment. Grant the lock helper only to SDK roles authorized to create push
notification configurations. The invoking push-store role needs task-table
`SELECT` plus lock-helper `EXECUTE`, but it does not need task-table `UPDATE`;
the helper's owner needs `UPDATE` because the `SECURITY DEFINER` function takes
`FOR KEY SHARE`. Push-only roles paired with an external authoritative
`TaskStore` do not need task-table access or lock-helper execution. PostgreSQL
row-level security on the task table is allowed for external-authority/push-only
use, but the local task-aware create path rejects it explicitly because a
`SECURITY DEFINER` lock must not bypass caller row policies.

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
