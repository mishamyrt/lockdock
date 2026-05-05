#include "lockdockd_preferences.h"

#include <lockdock_ipc.h>

#include <CoreFoundation/CoreFoundation.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define LOCKDOCKD_PREFERRED_UUID_KEY CFSTR("preferredDisplayUUID")
#define LOCKDOCKD_PREFERRED_BUILTIN_KEY CFSTR("preferredDisplayBuiltin")
#define LOCKDOCKD_PREFERRED_VENDOR_KEY CFSTR("preferredDisplayVendor")
#define LOCKDOCKD_PREFERRED_MODEL_KEY CFSTR("preferredDisplayModel")
#define LOCKDOCKD_PREFERRED_SERIAL_KEY CFSTR("preferredDisplaySerial")
#define LOCKDOCKD_PREFERENCES_DOMAIN CFSTR(LOCKDOCK_IPC_BUNDLE_ID)

static void lockdockd_set_error(char *buffer,
                                size_t buffer_size,
                                const char *message) {
    if (buffer == NULL || buffer_size == 0) {
        return;
    }

    snprintf(buffer, buffer_size, "%s", message);
}

static bool lockdockd_preferences_sync(char *error, size_t error_size) {
    if (CFPreferencesAppSynchronize(LOCKDOCKD_PREFERENCES_DOMAIN)) {
        return true;
    }

    lockdockd_set_error(error, error_size, "Failed to synchronize preferences");
    return false;
}

static void lockdockd_preferences_set_uint32(CFStringRef key, uint32_t value) {
    int64_t signed_value = (int64_t)value;
    CFNumberRef number =
        CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &signed_value);

    if (number == NULL) {
        CFPreferencesSetAppValue(key, NULL, LOCKDOCKD_PREFERENCES_DOMAIN);
        return;
    }

    CFPreferencesSetAppValue(key, number, LOCKDOCKD_PREFERENCES_DOMAIN);
    CFRelease(number);
}

static bool lockdockd_preferences_copy_uint32(CFStringRef key, uint32_t *value_out) {
    CFPropertyListRef value = NULL;
    int64_t signed_value = 0;

    if (value_out == NULL) {
        return false;
    }

    value = CFPreferencesCopyAppValue(key, LOCKDOCKD_PREFERENCES_DOMAIN);
    if (value == NULL) {
        return false;
    }

    if (CFGetTypeID(value) != CFNumberGetTypeID() ||
        !CFNumberGetValue((CFNumberRef)value, kCFNumberSInt64Type, &signed_value) ||
        signed_value < 0 || signed_value > UINT32_MAX) {
        CFRelease(value);
        return false;
    }

    *value_out = (uint32_t)signed_value;
    CFRelease(value);
    return true;
}

static bool lockdockd_preferences_copy_bool(CFStringRef key, bool *value_out) {
    CFPropertyListRef value = NULL;

    if (value_out == NULL) {
        return false;
    }

    value = CFPreferencesCopyAppValue(key, LOCKDOCKD_PREFERENCES_DOMAIN);
    if (value == NULL) {
        return false;
    }

    if (CFGetTypeID(value) != CFBooleanGetTypeID()) {
        CFRelease(value);
        return false;
    }

    *value_out = CFBooleanGetValue((CFBooleanRef)value);
    CFRelease(value);
    return true;
}

bool lockdockd_preferences_save_preferred_display(
    const LockDockdDisplayIdentity *identity,
    char *error,
    size_t error_size) {
    CFStringRef uuid = NULL;

    if (!lockdockd_display_identity_is_valid(identity)) {
        lockdockd_set_error(error, error_size, "Internal error");
        return false;
    }

    if (identity->uuid[0] != '\0') {
        uuid = CFStringCreateWithCString(kCFAllocatorDefault, identity->uuid,
                                         kCFStringEncodingUTF8);
        if (uuid == NULL) {
            lockdockd_set_error(error, error_size, "Failed to encode display UUID");
            return false;
        }
    }

    CFPreferencesSetAppValue(LOCKDOCKD_PREFERRED_UUID_KEY, uuid,
                             LOCKDOCKD_PREFERENCES_DOMAIN);
    CFPreferencesSetAppValue(LOCKDOCKD_PREFERRED_BUILTIN_KEY,
                             identity->is_builtin ? kCFBooleanTrue : kCFBooleanFalse,
                             LOCKDOCKD_PREFERENCES_DOMAIN);
    lockdockd_preferences_set_uint32(LOCKDOCKD_PREFERRED_VENDOR_KEY,
                                     identity->vendor_number);
    lockdockd_preferences_set_uint32(LOCKDOCKD_PREFERRED_MODEL_KEY,
                                     identity->model_number);
    lockdockd_preferences_set_uint32(LOCKDOCKD_PREFERRED_SERIAL_KEY,
                                     identity->serial_number);

    if (uuid != NULL) {
        CFRelease(uuid);
    }

    return lockdockd_preferences_sync(error, error_size);
}

bool lockdockd_preferences_load_preferred_display(
    LockDockdDisplayIdentity *identity_out) {
    CFPropertyListRef uuid_value = NULL;
    bool has_builtin = false;

    if (identity_out == NULL) {
        return false;
    }

    memset(identity_out, 0, sizeof(*identity_out));

    uuid_value = CFPreferencesCopyAppValue(LOCKDOCKD_PREFERRED_UUID_KEY,
                                           LOCKDOCKD_PREFERENCES_DOMAIN);
    if (uuid_value != NULL) {
        if (CFGetTypeID(uuid_value) == CFStringGetTypeID()) {
            CFStringGetCString((CFStringRef)uuid_value, identity_out->uuid,
                               sizeof(identity_out->uuid), kCFStringEncodingUTF8);
        }

        CFRelease(uuid_value);
    }

    if (lockdockd_preferences_copy_bool(LOCKDOCKD_PREFERRED_BUILTIN_KEY,
                                        &identity_out->is_builtin)) {
        has_builtin = true;
    }

    lockdockd_preferences_copy_uint32(LOCKDOCKD_PREFERRED_VENDOR_KEY,
                                      &identity_out->vendor_number);
    lockdockd_preferences_copy_uint32(LOCKDOCKD_PREFERRED_MODEL_KEY,
                                      &identity_out->model_number);
    lockdockd_preferences_copy_uint32(LOCKDOCKD_PREFERRED_SERIAL_KEY,
                                      &identity_out->serial_number);

    if (!lockdockd_display_identity_is_valid(identity_out)) {
        if (!has_builtin) {
            return false;
        }

        return identity_out->is_builtin;
    }

    return true;
}

bool lockdockd_preferences_clear_preferred_display(char *error, size_t error_size) {
    CFPreferencesSetAppValue(LOCKDOCKD_PREFERRED_UUID_KEY, NULL,
                             LOCKDOCKD_PREFERENCES_DOMAIN);
    CFPreferencesSetAppValue(LOCKDOCKD_PREFERRED_BUILTIN_KEY, NULL,
                             LOCKDOCKD_PREFERENCES_DOMAIN);
    CFPreferencesSetAppValue(LOCKDOCKD_PREFERRED_VENDOR_KEY, NULL,
                             LOCKDOCKD_PREFERENCES_DOMAIN);
    CFPreferencesSetAppValue(LOCKDOCKD_PREFERRED_MODEL_KEY, NULL,
                             LOCKDOCKD_PREFERENCES_DOMAIN);
    CFPreferencesSetAppValue(LOCKDOCKD_PREFERRED_SERIAL_KEY, NULL,
                             LOCKDOCKD_PREFERENCES_DOMAIN);
    return lockdockd_preferences_sync(error, error_size);
}
