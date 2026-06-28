# FetchContent consumer

Use this template when your application should build a2a-cpp from a Git repository as part of your CMake configure step.

```bash
cmake -S examples/fetch_content_consumer -B build-example -DA2A_EXAMPLE_APP=hello_agent
cmake --build build-example --parallel
./build-example/a2a_example
```

Select another app with `-DA2A_EXAMPLE_APP=<name>`. For reproducible builds, pin `A2A_CPP_GIT_TAG` to a release tag or commit instead of relying on the default branch:

```bash
cmake -S examples/fetch_content_consumer -B build-example \
  -DA2A_EXAMPLE_APP=rest_server \
  -DA2A_CPP_GIT_TAG=<release-tag-or-commit>
```
