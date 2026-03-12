#pragma once

// DPOR simulation support for the Two-Phase Commit example.
//
// The simulation code is split into:
// - sim/dpor_types.hpp: values and graph types exposed to DPOR
// - sim/bridge.hpp: conversion between protocol messages and DPOR values
// - sim/core.hpp: shared deterministic replay/capture helper
// - sim/*_environment.hpp: explicit scenario environments
// - sim/programs.hpp: scenario-specific ThreadFunction / Program builders

#include "sim/bridge.hpp"
#include "sim/core.hpp"
#include "sim/crash_environment.hpp"
#include "sim/dpor_types.hpp"
#include "sim/nominal_environment.hpp"
#include "sim/programs.hpp"
