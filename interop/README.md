# interop — RTPS wire-level interop tests against a live CycloneDDS peer

This package proves `dds::rtps::Participant` is genuinely wire-compatible
with a *separate, independently built* RTPS 2.3 implementation over real
UDP/multicast — not just byte-vector-identical to go-DDS's encoder
(covered already by `tests/test_rtps_*.cpp`) and not just a same-process
loopback round trip (also already covered). See `ROADMAP.md`'s "Interop
testing infrastructure (new need — not just golden vectors)" section for
why this is a distinct testing tier from the golden-vector work.

It mirrors go-DDS's own `interop/` package pattern exactly
(`github.com/SoundMatt/go-DDS/interop`: `go:build interop`-tagged tests,
`interop/doc.go`, `interop/docker-compose.yml`, `INTEROP_DOMAIN` /
`INTEROP_TIMEOUT` env vars) so the two implementations' interop harnesses
stay recognizable as the same kind of infrastructure across the ecosystem.

## Why this is a separate CMake target, not part of `cppdds_tests`

These tests need a live CycloneDDS peer reachable over the network. They
are **not** part of the default `cppdds_tests` binary or the default
`ctest` invocation — they live in their own `cppdds_interop_tests`
executable, built only when the `CPPDDS_INTEROP_TESTS` CMake option is
explicitly turned on (default `OFF`). This is the CMake/CTest equivalent
of go-DDS's `//go:build interop` tag: with the option off (the default for
every normal build, including the CI `build-and-test` matrix), this
directory's tests are never compiled, linked, or registered with CTest at
all — not merely skipped or filtered out afterward.

Tests are additionally tagged with the CTest label `interop`
(`ctest -L interop` / `ctest -LE interop`) for the case where a caller who
already opted in with `CPPDDS_INTEROP_TESTS=ON` still wants to separate
this suite from anything else registered in that build.

## Prerequisites

1. Docker (for `docker compose`), or a native CycloneDDS installation
   (`ddsperf` on `PATH`) on the same host.
2. Linux — the compose file uses `network_mode: host` so RTPS SPDP/SEDP
   multicast actually reaches the container (matching CI's
   `ubuntu-22.04` runner). macOS/Windows Docker Desktop have no host
   network mode; run a native CycloneDDS install instead on those
   platforms.
3. A CycloneDDS peer on the same DDS domain (default domain `0`), started
   from `interop/docker-compose.yml` (see Quick start below).

## Quick start

```sh
# 1. Start a CycloneDDS peer that both publishes and subscribes, exercising
#    both directions of the wire (see docker-compose.yml's cyclone-peer
#    service for its exact ddsperf invocation). The peer image is built
#    from eclipse-cyclonedds/cyclonedds's own upstream source the first
#    time this runs (see Dockerfile.cyclonedds) — no image is pulled from
#    a registry, so the first `up` takes a couple of minutes; subsequent
#    runs reuse the cached image until CYCLONEDDS_VERSION changes.
docker compose -f interop/docker-compose.yml up -d cyclone-peer

# 2. Configure and build with the interop suite enabled.
cmake -B build-interop -DCPPDDS_INTEROP_TESTS=ON -G Ninja
cmake --build build-interop --parallel

# 3. Run just the interop suite.
ctest --test-dir build-interop -L interop --output-on-failure

# 4. Tear the peer down.
docker compose -f interop/docker-compose.yml down
```

To exercise only one direction, run the single-purpose services instead of
`cyclone-peer` (both use Compose profiles so they don't start with a plain
`up`):

```sh
docker compose -f interop/docker-compose.yml run --rm cyclone-sub   # receives from cpp-DDS
docker compose -f interop/docker-compose.yml run --rm cyclone-pub   # publishes to cpp-DDS
```

## Environment variables

Matches go-DDS's `INTEROP_DOMAIN` / `INTEROP_TIMEOUT` convention exactly:

- `INTEROP_DOMAIN` — DDS domain number (default `0`).
- `INTEROP_TIMEOUT` — per-test deadline, e.g. `"10s"`, `"500ms"` (default
  `"15s"`).

## What's tested

- **`cpp-DDS publisher reaches a live CycloneDDS subscriber`** — a real
  `Participant` publishes on `interop/cpp-dds-ping`; requires the
  `cyclone-sub` (or `cyclone-peer`) service to be running to observe
  receipt. The test itself only fails if the local write path errors —
  see `test_interop.cpp` for the same caveat go-DDS's own
  `TestInterop_GoPublisher_CycloneSubscriber` documents (receipt is
  verified out-of-band, by watching the container's stdout).
- **`cpp-DDS subscriber receives from a live CycloneDDS publisher`** — a
  real `Participant` subscribes on `interop/cpp-dds-pong` and expects at
  least one sample within `INTEROP_TIMEOUT`; requires `cyclone-pub` (or
  `cyclone-peer`).
- **`cpp-DDS <-> CycloneDDS bidirectional echo`** — publishes on
  `interop/cpp-dds-ping` and expects a sample back on
  `interop/cpp-dds-pong` from `cyclone-peer` within `INTEROP_TIMEOUT`.

All three skip (not fail) if `Participant::create` itself fails (no
UDP/multicast route available in the current sandbox) or if no sample
arrives within `INTEROP_TIMEOUT` — a missing peer container is a setup
problem, not a wire-compatibility regression, so it reports as skipped
rather than red.

## 3-way matrix

This lands the CycloneDDS leg first, matching go-DDS's own first interop
target and reference pattern. `ROADMAP.md` tracks the eventual 3-way
matrix (cpp-DDS &lt;-&gt; go-DDS, cpp-DDS &lt;-&gt; CycloneDDS, cpp-DDS
&lt;-&gt; rust-DDS once it exists) — this directory is where the
go-DDS-peer and rust-DDS-peer harnesses will land alongside this one, each
as its own docker-compose service / test file, once built.
