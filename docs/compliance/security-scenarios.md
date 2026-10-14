# SDK security scenarios

The TCK SUT's `security` profile uses one SDK dispatcher, security policy, task
store and executor shared by JSON-RPC, HTTP+JSON and gRPC. It supplies two
identities, a small permission policy, credential/key fixtures and an
application trigger. Security enforcement, authorization lifecycle, streams,
TLS clients and card signing/verification reside in public SDK APIs.

```bash
./scripts/install_build_deps.sh
./scripts/install_security_test_deps.sh
cmake -S . -B build -DA2A_ENABLE_POSTGRES_STORE=ON -DA2A_ENABLE_SIGNING=ON
cmake --build build --parallel 2
A2A_TEST_POSTGRES_DSN=postgresql://a2a:a2a@127.0.0.1:5432/a2a \
  ctest --test-dir build -L security --output-on-failure
```

Signing is disabled by default. Ordinary SDK builds do not require nginx or the
OpenSSL CLI. The signing-enabled profile above includes signed-card and live
security scenarios; canonicalization, permissions and task-authorization tests
also run with signing disabled. For canonical contributor validation use
`A2A_VERIFY_SIGNING=ON` with `./scripts/verify_changes.sh`.

The independent SDK unit and adapter integration tests run through CTest on
supported platforms. Live SUT scenarios require Unix, Python, libcurl, nginx and
the OpenSSL CLI and are registered when those tools are present. The PostgreSQL
scenario requires `A2A_TEST_POSTGRES_DSN`; its absence produces an explicit skip.
Each launcher run uses temporary credential/CA files, free loopback ports and an
isolated database schema which it removes afterward.

The live matrix covers both stores, all nine ordered binding pairs, and plaintext
local, trusted TLS, unrelated CA, expired certificate and hostname-mismatch
profiles. All tests use public C++ SDK clients. Authorization tests retain both
the send stream and a separate subscription while receiving a pending message,
then resume the same task after the local credential file is written.

For manual profile selection set:

```bash
unset A2A_TCK_AUTH_TOKEN
export A2A_TCK_PROFILE=security
export A2A_TCK_ALICE_TOKEN='<caller fixture A>'
export A2A_TCK_BOB_TOKEN='<caller fixture B>'
export A2A_TCK_CREDENTIAL_DIRECTORY='<private fixture directory>'
export A2A_TCK_TASK_CREDENTIAL='<out-of-band credential fixture>'
./scripts/run_tck_sut.sh
```

New messages use `contextId=alice` or `contextId=bob` with the matching fixture
identity. Send a `request-authorization` text part to create an authorization-
pending task. Put the task credential in a file named after the returned task ID
in the configured directory to authorize and complete it. The two caller tokens
must be nonempty and different. These are deterministic application policies,
not a production identity system.

The TCK CI workflow also executes the SDK security CTest label, including both
stores, separately from the unmodified upstream mandatory runner. No local
Python TCK test modules or compliance report gates are added.

At upstream revision `263b9cfaf16a554bdfb166a7ba5b67716e946349`, the authentication,
authorization, TLS and card-signing groups are registered without executable
assertions. They remain **NOT TESTED** in upstream compatibility reports.
These C++ tests are requirement-mapped SDK evidence and do not raise the upstream
percentage. Upstream test/fixture integration is tracked by
[a2a-cpp #305](https://github.com/MisterVVP/a2a-cpp/issues/305),
[a2a-tck task 27](https://github.com/a2aproject/a2a-tck/blob/main/backlog/tasks/task-27%20-%20Add-auth-security-conformance-tests-AUTH.md),
[a2a-tck task 29](https://github.com/a2aproject/a2a-tck/blob/main/backlog/tasks/task-29%20-%20Add-agent-card-signing-conformance-tests-CARD-SIGN.md),
and the upstream `GRPC-SVC-003` coverage backlog.

- [Caller permissions](security-permissions.md): `AUTH-SERVER-002`, `AUTH-SCOPE-001..003`
- [Task authorization](task-authorization.md): `AUTH-INTASK-001..006`
- [TLS and server identity](security-tls.md): `AUTH-TLS-001/002`, `AUTH-SERVER-001`, `GRPC-SVC-003`
- [Signed cards](card-signing.md): `CARD-SIGN-001..004`
