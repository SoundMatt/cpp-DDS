# Contributing to cpp-DDS

## Requirements before any PR

- Builds successfully (`cmake --build build --parallel`)
- Tests pass (`ctest --test-dir build --output-on-failure`)
- No new warnings under `-Wall -Wextra -Wpedantic`
- All new behavior is traced to a requirement in `requirements/requirements.json`
- DCO sign-off on every commit (`git commit -s`)

## Branch workflow

```bash
git checkout main && git pull
git checkout -b feat/<area>-<short>
# implement + tests
cmake -B build -DCMAKE_BUILD_TYPE=Release -G Ninja
cmake --build build --parallel
ctest --test-dir build --output-on-failure
git add ...
git commit -s -m "feat(area): short summary"
git push -u origin feat/<area>-<short>
gh pr create --base main
```

## Commit style

```
type(scope): short summary

Body explaining *why*, not what.

Signed-off-by: Your Name <you@example.com>
```

Types: `feat`, `fix`, `test`, `refactor`, `docs`, `ci`.

## Adding a new transport

1. Create `include/dds/<transport>/participant.hpp` and `src/<transport>/participant.cpp`.
2. Add a `REQ-DDS-xxx` requirement for each behavioral guarantee.
3. Add a `test_<transport>.cpp` covering all requirements.
4. Add the source file to `CMakeLists.txt` under an `option(CPPDDS_<TRANSPORT> ...)` guard.
5. Update `ROADMAP.md`.

## Requirements discipline

- Every public behavior must map to a `REQ-DDS-xxx` or `REQ-MOCK-xxx` in `requirements/requirements.json`.
- Annotate implementations with `// fusa:req REQ-DDS-xxx`.
- Annotate tests with `// fusa:test REQ-DDS-xxx`.
- Never renumber or reuse requirement IDs.

## Test patterns

- Use `dds::mock::create()` for unit tests — zero OS dependencies.
- Use `relay::Context::with_timeout()` to bound any blocking test.
- Prefer `REQUIRE` for preconditions that must hold; `CHECK` for assertions within a passing test.
