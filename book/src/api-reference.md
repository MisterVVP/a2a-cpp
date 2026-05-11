# API Reference

The C++ API reference is generated from public headers under `include/a2a/**` using Doxygen and is published with this documentation site.

## Open the generated reference

- [C++ API Reference (generated)](api/cpp/index.html)

## Coverage

The generated API includes the public interfaces for:

- Core primitives (`a2a::core`)
- Client APIs and transports (`a2a::client`)
- Server APIs and transports (`a2a::server`)
- Authentication-related client hooks (`a2a::client::auth`)

## Local regeneration

From the repository root:

```bash
./scripts/generate_api_reference.sh
```

By default this writes generated pages to `book-build/api/cpp`.
