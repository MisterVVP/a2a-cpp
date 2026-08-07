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
matching uses libpq's logical connection target rather than the active resolved
server IP, so DNS and multi-host failover do not invalidate pool identity.
Explicit `hostaddr` and `target_session_attrs` differences remain distinct.

## Externally managed PostgreSQL schemas

When `auto_create_schema=false`, the push-notification store validates the
`task-aware-push-config-v2` migration during construction. Validation covers the
exact provenance column/default, removal of the legacy task foreign key,
constrained SECURITY DEFINER helpers and owner privileges, absence of `PUBLIC
EXECUTE`, the cleanup implementation/version, and the enabled `AFTER DELETE`
row trigger with no `WHEN` clause. Startup fails before serving requests when
any required object is absent or incorrectly configured.

Apply the following migration before upgrading. The example uses the `public`
schema and an SDK role named `a2a_sdk`; replace both names for your deployment.
Grant the lock helper only to roles authorized to create push configurations.
Task-table `SELECT` and lock-helper `EXECUTE` are required only for roles that
use local PostgreSQL task-aware creation. Push-only roles paired with an
external authoritative `TaskStore` do not need them. Task-table row-level
security is allowed for those external-authority paths but is rejected when the
local task-aware create helper is invoked.

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
