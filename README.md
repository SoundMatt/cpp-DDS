# cpp-DDS

A C++17 library for [Data Distribution Service (DDS)](https://www.omg.org/spec/DDS/About-DDS/) publish/subscribe communication.
RELAY-conformant — the `dds::IParticipant` interface is stable; transports are swappable without changing application code.

[![CI](https://github.com/SoundMatt/cpp-DDS/actions/workflows/ci.yml/badge.svg)](https://github.com/SoundMatt/cpp-DDS/actions/workflows/ci.yml)

## Packages

| Header | Description | Dependencies |
|--------|-------------|--------------|
| `dds/dds.hpp` | Core `IParticipant`, `IPublisher`, `ISubscriber` interfaces, `Sample`, `QoS` | Nothing |
| `dds/mock/participant.hpp` | In-process broadcast participant — zero OS deps, default for testing | `dds/dds.hpp` |
| `dds/relay.hpp` | RELAY spec types (`relay::Message`, `relay::INode`, error sentinels) | Nothing |
| `dds/channel.hpp` | `relay::Channel<T>` — bounded, thread-safe FIFO channel (`dds::Chan<T>` alias) | Nothing |

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Requires CMake ≥ 3.21 and a C++17-compliant compiler. Dependencies are fetched automatically via CMake FetchContent (Catch2).

## Quick start

```cpp
#include <dds/dds.hpp>
#include <dds/mock/participant.hpp>

auto [p, ec] = dds::mock::create(0);  // domain 0

auto [sub, es] = p->new_subscriber("vehicle/speed", dds::default_qos());
auto [pub, ep] = p->new_publisher("vehicle/speed", dds::default_qos());

pub->write({0xDE, 0xAD, 0xBE, 0xEF});

if (auto sample = sub->channel()->recv()) {
    // sample->topic, sample->payload, sample->sequence_number
}
p->close();
```

## Switching transports

```cpp
// Development / testing — zero dependencies:
#include <dds/mock/participant.hpp>
auto [p, ec] = dds::mock::create(0);

// RTPS/UDP — wire-compatible with CycloneDDS (future: dds/rtps/participant.hpp)
// CycloneDDS CGo — full OMG DDS (future: dds/cyclone/participant.hpp)
```

## QoS

```cpp
// BestEffort + Volatile (default)
auto q = dds::default_qos();

// Reliable + TransientLocal — late subscribers receive the last sample
auto q = dds::reliable_qos();

// Custom
dds::QoS q;
q.reliability    = dds::ReliabilityKind::Reliable;
q.durability     = dds::DurabilityKind::TransientLocal;
q.max_sample_size = 1024;  // returns ErrPayloadTooLarge if exceeded
```

## RELAY bridge

```cpp
#include <dds/dds.hpp>
#include <dds/mock/participant.hpp>

auto [p, _] = dds::mock::create(0);
auto node = dds::adapt(p);  // node implements relay::INode

relay::Message msg;
msg.id      = "vehicle/speed";
msg.payload = {0x01, 0x02};
node->send(msg);

auto [ch, ec] = node->subscribe({relay::with_topic("vehicle/speed")});
```

## Error handling

All functions return `std::error_code`. DDS-specific errors map to relay sentinels:

| Error | relay sentinel |
|-------|---------------|
| `ErrTopicEmpty` | `ErrNotConnected` |
| `ErrQoSMismatch` | `ErrNotConnected` |
| `ErrDeadlineMissed` | `ErrTimeout` |
| `ErrSampleRejected` | `ErrPayloadTooLarge` |
| `ErrResourceLimits` | `ErrPayloadTooLarge` |
| `ErrLoanBuffer` | `ErrClosed` |
| `ErrDomainOutOfRange` | `ErrNotConnected` |

## CLI

```bash
./build/cpp-dds version
./build/cpp-dds conform

# §11.2 convert: reads a dds.Sample JSON value on stdin, writes the
# resulting relay.Message JSON on stdout.
echo '{
  "topic": "vehicle/speed", "payload": "3q2+7w==",
  "timestamp": "2026-01-01T00:00:00Z", "seq": 1,
  "writer_guid": [0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]
}' | ./build/cpp-dds convert --protocol DDS
```

## Safety evidence (RELAY spec §20.4)

| Evidence | File |
|---|---|
| Requirements registry | [`requirements/requirements.json`](requirements/requirements.json) |
| HARA (hazard analysis and risk assessment) | [`HARA.md`](HARA.md) / [`.fusa-hara.json`](.fusa-hara.json) |
| TARA (ISO/SAE 21434 threat analysis) | [`tara.md`](tara.md) / [`tara.json`](tara.json) |
| dFMEA (generated failure-mode analysis) | [`fmea.csv`](fmea.csv) / [`fmea.json`](fmea.json) |

Regenerate TARA/dFMEA with `cpfusa tara --dir .` / `cpfusa fmea --dir .`
(from [cpp-FuSa](https://github.com/SoundMatt/cpp-FuSa)) after source changes;
HARA is hand-maintained in `.fusa-hara.json` and validated by `cpfusa check`.
CI (`.github/workflows/ci.yml` `fusa-asil-b` job) additionally gates on the
ISO 21434 cybersecurity analysis (`cpfusa cyber`), a dependency vulnerability
scan (`cpfusa vuln`), and the tool qualification suite (`cpfusa qualify`) per
RELAY spec §20.1; the `relay-conform` job gates on `relay interop --protocol
DDS --strict` (§20.1 item 3 / §20.2 behavioural conformance). Release tags
additionally get an SBOM and build provenance attached by
`.github/workflows/release.yml` per §20.5.

## License

Mozilla Public License Version 2.0. See [LICENSE](LICENSE).
