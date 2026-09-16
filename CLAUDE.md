# Gloo

Gloo is a C++20 collective communications library.
Keep fixes minimal and preserve compatibility. See `CONTRIBUTING.md` and `docs/`.

## Layout

- `gloo/`: collective algorithms.
- `gloo/transport/`: transport implementations.
- `gloo/rendezvous/`: process coordination.
- `gloo/test/`: Google Test suites.

## Build and test

CPU tests require CMake 3.21+, a C++20 compiler, and Google Test.

```sh
cmake -S . -B build -DBUILD_TEST=ON -DUSE_CUDA=OFF -DUSE_ROCM=OFF
cmake --build build --parallel
./build/gloo/test/gloo_test
```

Enable optional transports or GPU backends only when needed for the change.

## Lint

CI runs Super-linter on changed files via `.github/workflows/super-linter.yml`.
With Docker, run from the repository root:

```sh
docker run --rm \
  -e RUN_LOCAL=true \
  -e VALIDATE_ALL_CODEBASE=false \
  -e DEFAULT_BRANCH=main \
  -e LINTER_RULES_PATH=.github/config/lint \
  -e VALIDATE_JSCPD=false \
  -e VALIDATE_CPP=false \
  -v "$(pwd):/tmp/lint" \
  ghcr.io/super-linter/super-linter:v8.3.2
```

The local `main` ref is the comparison base.
Follow `.clang-format`: two-space indentation, 80-column lines, and camelCase.

## Review

Check collective ordering across ranks, matching sends and receives, buffer
lifetimes, timeouts, and error paths for deadlocks. Add regression tests to the
general suites unless the behavior is specific to a backend.
