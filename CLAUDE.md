# cpp-DDS — Claude session guide

Repo: `github.com/SoundMatt/cpp-DDS`
Local path: `/Users/matt/Documents/Coding/SoundMatt/cpp-DDS`

## Project overview

A certified, multi-industry C++17 DDS (Data Distribution Service) pub/sub library targeting
automotive, aerospace, medical, industrial, robotics, and cloud deployments.
RELAY spec v1.7 conformant.

| File/Directory | What it is |
|---|---|
| `include/dds/dds.hpp` | `IParticipant`, `IPublisher`, `ISubscriber`, `Sample`, `QoS`, errors |
| `include/dds/relay.hpp` | `relay::INode`, `relay::Message`, `relay::Context`, error sentinels |
| `include/dds/channel.hpp` | `dds::Chan<T>` — bounded, thread-safe FIFO channel |
| `include/dds/mock/participant.hpp` | In-process mock participant header |
| `src/relay.cpp` | relay error category, to_string, parse_protocol |
| `src/dds.cpp` | DDS error category, Sample::to_message, from_message, adapt() |
| `src/mock/participant.cpp` | Broker, MockPublisher, MockSubscriber, MockParticipant |
| `cli/main.cpp` | `cpp-dds` CLI — version / conform / convert |
| `requirements/requirements.json` | Machine-readable requirements (REQ-DDS-xxx, REQ-MOCK-xxx) |
| `tests/` | test_relay.cpp, test_dds.cpp, test_mock.cpp |

## Per-PR checklist

1. `git checkout main && git pull origin main`
2. `git checkout -b fix/<area>-<short>` or `feat/<area>-<short>`
3. Implement + tests. Annotate with `// fusa:req` and `// fusa:test`.
4. `cmake -B build -DCMAKE_BUILD_TYPE=Release -G Ninja`
5. `cmake --build build --parallel`
6. `ctest --test-dir build --output-on-failure -j1`
7. Commit with DCO sign-off.
8. `git push origin <branch>`, open PR targeting `main`.
9. Wait for all CI checks green, then merge (squash).

## Commit message style

```
type(scope): short summary

Body explaining *why*, not what.

Signed-off-by: Matt Jones <matt@jellybaby.com>
```

Use heredoc to avoid shell expansion issues:
```bash
git commit -F - <<'COMMIT'
feat(mock): add TransientLocal late-join delivery

Signed-off-by: Matt Jones <matt@jellybaby.com>
COMMIT
```

## C++ conventions for this repo

- C++17 minimum; no C++20 features unless guarded by `__cplusplus >= 202002L`.
- All public errors returned as `std::error_code`; never throw across API boundaries.
- `dds::Chan<T>` is non-copyable, non-movable; always heap-allocate via `shared_ptr`.
- Implementations annotate with `// fusa:req REQ-DDS-xxx`; tests with `// fusa:test REQ-DDS-xxx`.
- Never throw from `close()`, `unsubscribe()`, or destructors.
- The mock Broker is process-global (static). Tests using the same topic name share state;
  use unique topic names per test case to avoid cross-contamination.

## RELAY spec

The canonical reference is `/Users/matt/Documents/Coding/SoundMatt/RELAY/spec/relay-spec.md`.
DDS section: §8.2 (interface contract), §15.2 (canonical types), §15.7.2 (to/from message).

## go-DDS reference

The Go implementation lives at `/Users/matt/Documents/Coding/SoundMatt/go-DDS`.
Use it as behavioral reference when implementing a new feature.

## Autonomous operation

Matt has granted blanket permission for all cmake/ctest/git/gh operations in this repo.
No confirmation needed for building, testing, or committing.
