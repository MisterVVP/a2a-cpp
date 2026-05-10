# Task 10 — Auth and security hooks

## Goal

Add the auth abstraction layer and metadata handling needed for real-world SDK use without overbuilding every provider-specific flow.

## Scope

- Parse and expose Agent Card security metadata.
- Add client-side credential provider hooks.
- Support request decoration for:
  - API key
  - Bearer token
  - custom header auth
  - mTLS configuration surface
- Add server-side auth metadata extraction into `RequestContext`.
- Add extension points for future OAuth2 helpers without implementing full flows.

## Constraints

- A2A **1.0 only**.
- This task is about hooks and abstractions, not a full identity product.
- Avoid hardwiring provider-specific OAuth logic.

## Deliverables

- credential provider interfaces
- request signer / decorator hooks
- server-side auth metadata extraction
- tests for header injection and context propagation
- documentation for supported auth patterns

## Suggested interfaces

```cpp
class CredentialProvider {
public:
  virtual Result<AuthHeaders> GetHeaders(const AuthContext&) = 0;
  virtual ~CredentialProvider() = default;
};
```

## Implementation notes

- Auth should be transport-agnostic at the API boundary, even if applied differently underneath.
- Keep mTLS config separate from header-based auth.
- Preserve enough metadata in `RequestContext` for user code to make authorization decisions.

## Test expectations

Cover:
- API key header injection
- bearer token header injection
- custom header injection
- mTLS config plumbed into transport config
- server context receives auth metadata

## Acceptance criteria

- A user can provide credentials without forking transport code.
- Server-side executor receives auth-related metadata in context.
- Security metadata from Agent Card is accessible through the SDK.

## Out of scope

- complete OAuth2 interactive flows
- policy engine
- secrets management product features
