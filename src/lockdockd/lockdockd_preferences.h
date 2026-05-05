#ifndef LOCKDOCKD_PREFERENCES_H
#define LOCKDOCKD_PREFERENCES_H

#include "lockdockd_display.h"

#include <stdbool.h>
#include <stddef.h>

bool lockdockd_preferences_save_preferred_display(
    const LockDockdDisplayIdentity *identity,
    char *error,
    size_t error_size);
bool lockdockd_preferences_load_preferred_display(
    LockDockdDisplayIdentity *identity_out);
bool lockdockd_preferences_clear_preferred_display(char *error, size_t error_size);

#endif
