#ifndef LOCKDOCK_PREFERENCES_H
#define LOCKDOCK_PREFERENCES_H

#include "lockdock_display.h"

#include <stdbool.h>
#include <stddef.h>

bool lockdock_preferences_save_preferred_display(
    const LockDockDisplayIdentity *identity,
    char *error,
    size_t error_size);
bool lockdock_preferences_load_preferred_display(
    LockDockDisplayIdentity *identity_out);
bool lockdock_preferences_clear_preferred_display(char *error, size_t error_size);

#endif
