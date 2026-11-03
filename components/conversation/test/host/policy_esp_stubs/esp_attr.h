// Host-only interface double. Does not validate ESP memory placement.
#pragma once
#define DRAM_ATTR __attribute__((section(".dram1.policy_test")))
