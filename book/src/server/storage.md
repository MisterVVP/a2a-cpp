# Storage Backends

The SDK storage layer is pluggable. `TaskLifecycleService` uses `TaskStore`, and
`PushNotificationService` uses `PushNotificationStore`.

## In-memory stores

Use `InMemoryStoreFactory` for examples, tests, and single-process development:

```cpp
a2a::server::stores::InMemoryStoreFactory factory;
auto stores = factory.CreateStoreBundle();
```

In-memory stores do not persist data or coordinate across processes.

## PostgreSQL stores

PostgreSQL support is optional. Enable and link the PostgreSQL store target:

```bash
cmake -S . -B build-postgres -DA2A_ENABLE_POSTGRES_STORE=ON
cmake --build build-postgres
```

```cpp
a2a::server::stores::PostgresStoreFactory factory({
    .connection_string = "postgresql://a2a:a2a@127.0.0.1:5432/a2a",
    .schema = "public",
    .auto_create_schema = true,
    .connection_pool_size = 8,
});
auto stores = factory.CreateStoreBundle();
```

Task and push-notification stores returned by `CreateStoreBundle()` share one
connection pool. Separately constructed stores own separate pools. Storage
matching uses libpq's active connection target (selected host, resolved server
address, active port, and database) plus the configured `target_session_attrs`
value and schema. Exact coordinate equality confirms local authority; a
different database or schema confirms external authority; other endpoint
differences are uncertain. Separately constructed stores can set
`PostgresStoreOptions::storage_authority_id`: matching non-empty IDs prove local
authority, different non-empty IDs prove external authority, and a one-sided ID
remains uncertain. Database/schema differences still remain external. The ID is
a stable, non-secret deployment identifier and must reflect the real storage
authority because it affects provenance and cleanup. Passwords and raw
connection strings are not part of the identity. The effective role is tracked
separately: role differences still use the one-command local create path, while
the one-command list shortcut requires identical storage coordinates and role.

### Task-aware PostgreSQL behavior

For same-storage task and push stores, create/update uses one push-store
acquisition and one PostgreSQL command. That statement calls the
`SECURITY DEFINER` task-lock helper, holds `FOR KEY SHARE`, and performs the
upsert atomically. It has no separate task precheck, revalidation, compensating
cleanup, or explicit multi-command transaction. Missing tasks return
`TaskNotFound`, and concurrent deletion cannot leave a locally owned orphan.
`PushConfig_CreateMany` at fan-out eight therefore performs eight create
commands rather than sixteen.

`GetConfig` remains push-store-only and uses one PostgreSQL statement. List uses
one combined task/count/page statement when execution identities match; a
different role or external task store uses the authoritative task lookup first
and then one combined push-list statement. External-authority creates are marked
with `local_postgres_task=FALSE`; direct non-task-aware `CreateOrUpdate` calls
use the same external provenance. Only locally owned rows are removed by the
task-delete cleanup trigger.

When `auto_create_schema=true`, the SDK installs the provenance column, lock and
cleanup helpers, `AFTER DELETE` trigger, sequences, and indexes, removes the
legacy push-to-task foreign key, and writes the `task-aware-push-config-v2`
migration markers last.

## Externally managed PostgreSQL schemas

When `auto_create_schema=false`, the push-notification store always validates
the provenance column and absence of the legacy push-to-task foreign key. If the
lock helper, cleanup helper, and cleanup trigger are all absent, construction is
allowed for push-only use with an external authoritative `TaskStore`; local
PostgreSQL task-aware creation then fails before writing until the migration is
installed. Capability detection happens during store construction rather than
on the request path.

If any task-aware helper or trigger is present, the whole
`task-aware-push-config-v2` migration is required. Validation checks both helper
implementations and markers, owner privileges, absence of `PUBLIC EXECUTE`, the
enabled `AFTER DELETE` row trigger without a `WHEN` clause, and the cleanup
owner's ability to bypass any row-level security enabled on the push-config
table. Partial or stale installations fail construction.

Apply the following migration before using local PostgreSQL task-aware
create/update. The example uses the `public` schema and an SDK role named
`a2a_sdk`; replace both names for your deployment.
Grant the lock helper only to roles authorized to create push configurations.
The invoking push role needs task-table `SELECT` plus helper `EXECUTE`; task
`UPDATE` is required by the helper owner, not by every caller. Push-only roles
paired with an external authoritative `TaskStore` do not need task-table access
or helper execution. Task-table row-level security is allowed for those
external-authority paths but is rejected when the local task-aware create helper
is invoked. If push-table row-level security is enabled, the cleanup helper owner
must bypass it via table ownership without `FORCE ROW LEVEL SECURITY`, or via
`BYPASSRLS` or superuser status, so task deletion cannot silently retain local
push configurations.

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

The migration markers are written last inside the transaction, so partial
migrations are rejected. Existing rows receive the conservative external or
unknown provenance value from the new column's `FALSE` default. The lock-helper
owner needs schema `USAGE` plus task-table `SELECT` and `UPDATE`; the cleanup
helper owner needs schema `USAGE` plus push-table `SELECT` and `DELETE`. These
are SECURITY DEFINER owner requirements, not privileges for every push-store role.

## Sensitive push-notification data

Push configurations can contain callback URLs, tokens, and authentication
metadata. The SDK does not implement application-level encryption at rest; use
database encryption, least-privilege access, protected backups, and appropriate
audit logging.
