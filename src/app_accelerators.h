#ifndef TINKEREDIT_APP_ACCELERATORS_H
#define TINKEREDIT_APP_ACCELERATORS_H

#include <stddef.h>
#include <windows.h>

/* Upper bound used by create_main_accelerators() for its stack array. */
#define APP_FIXED_ACCELERATOR_MAX 64

const ACCEL *app_fixed_accelerators(size_t *count);

#endif
