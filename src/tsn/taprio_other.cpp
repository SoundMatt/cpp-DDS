// Copyright (c) 2026 Matt Jones. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// Non-Linux stub implementation of TAPRIOConfig::apply()/verify_applied().
// C++ port of github.com/SoundMatt/go-DDS's tsn/taprio_stub.go. This file
// is added to the build on every platform other than Linux — see
// CMakeLists.txt — mirroring go-DDS's `//go:build !linux` split and this
// repo's own traffic_linux.cpp/traffic_other.cpp precedent.
//
// TAPRIO qdisc configuration is a Linux `tc`/netlink concept with no
// macOS or Windows equivalent; on those platforms these functions return
// a "not supported" diagnostic so the rest of the code compiles and runs
// (config building, validation, and tc_command() template generation stay
// fully available everywhere — see taprio.cpp).

#include <dds/tsn/taprio.hpp>

namespace dds::tsn {

std::optional<std::string> TAPRIOConfig::apply() const {
    return std::string("tsn: TAPRIO qdisc requires Linux");
}

std::optional<std::string> TAPRIOConfig::verify_applied() const {
    return std::string("tsn: TAPRIO qdisc requires Linux");
}

} // namespace dds::tsn
