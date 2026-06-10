# Local SDK development

This directory contains local development helpers for the C++ SDK.

## PostgreSQL store

The PostgreSQL store is optional. A default build does not require PostgreSQL.

Start the local PostgreSQL container:

```bash
./dev/local/postgres_up.sh
```

Equivalent direct Docker Compose command:

```bash
docker compose -f dev/local/docker-compose.postgres.yml up -d
```

Configure and build with PostgreSQL support:

```bash
cmake -S . -B build-postgres \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DA2A_ENABLE_TESTING=ON \
  -DA2A_ENABLE_POSTGRES_STORE=ON
cmake --build build-postgres -j"$(nproc)"
```

Run PostgreSQL-enabled tests:

```bash
A2A_TEST_POSTGRES_DSN="postgresql://a2a:a2a@127.0.0.1:5432/a2a" ctest --test-dir build-postgres --output-on-failure
```

