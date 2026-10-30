#ifndef SETTINGS_H
#define SETTINGS_H

/* Umbrella header kept for the application shell (tinkeredit.c), which uses the
   persisted values, the load/save surface and the Find dialog state together.
   Smaller modules should include the narrower header they actually need:
   settings_values.h (persisted values + metrics), settings_io.h (load/save) or
   find_state.h (transient Find/Replace state). */

#include "settings_values.h"
#include "settings_io.h"
#include "find_state.h"

#endif
