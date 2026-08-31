#pragma once

// Keep the Seeed nRF52 TinyUSB defaults except for MSC.  G0 must enumerate as
// CDC only and must not even link the TinyUSB mass-storage class driver.
#include "arduino/ports/nrf/tusb_config_nrf.h"
#undef CFG_TUD_MSC
#define CFG_TUD_MSC 0
