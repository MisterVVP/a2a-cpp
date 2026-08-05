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
The PostgreSQL store requires libpq/PostgreSQL client version 12 or newer; CMake fails configuration with a clear error when an older client library is found.

Task-aware push-config creation uses one SQL command only when the push-config store and task store share the same PostgreSQL storage and execution identity. Split-role or external authoritative task-store deployments validate task existence first and then write the config without claiming cross-role atomicity; use a local PostgreSQL task store for delete-cascade cleanup guarantees. Existing foreign keys on push-config tables are preserved by automatic schema initialization.

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

## Sensitive push notification data

Push notification configs can include delivery URLs, tokens, and authentication metadata. Treat stored `TaskPushNotificationConfig` payloads as sensitive operational data. This SDK stores protobuf payloads in PostgreSQL but does not implement encryption at rest; use database-level encryption, access control, backups, and audit logging appropriate for your environment.
