// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
//
// Umbrella header. Segment Routing Fabric is the explicit governed segment-list
// construction, validation, lifecycle and authority runtime of the Distributed
// Fabric Infrastructure stack. Every shipped profile is ABSTRACT/SYNTHETIC: this
// runtime performs no physical segment-routing programming and claims no physical
// SR-MPLS or SRv6 support.
#pragma once

#include "srf/authority.hpp"
#include "srf/bytes.hpp"
#include "srf/canonical.hpp"
#include "srf/coordinator.hpp"
#include "srf/digest.hpp"
#include "srf/evidence.hpp"
#include "srf/ids.hpp"
#include "srf/lifecycle.hpp"
#include "srf/limits.hpp"
#include "srf/list.hpp"
#include "srf/persistence.hpp"
#include "srf/policy.hpp"
#include "srf/process.hpp"
#include "srf/profile.hpp"
#include "srf/reason.hpp"
#include "srf/registry.hpp"
#include "srf/segment.hpp"
#include "srf/snapshot.hpp"
#include "srf/store.hpp"
#include "srf/strong.hpp"
#include "srf/transport.hpp"
#include "srf/validation.hpp"
#include "srf/version.hpp"
#include "srf/wire.hpp"
#include "srf/worker.hpp"
