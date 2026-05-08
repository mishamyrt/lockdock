#include "lockdock_preferences.h"

#include "lockdock_config.h"
#include "lockdock_display.h"

#include <CoreFoundation/CoreFoundation.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef LOCKDOCK_TESTING
#include <stdlib.h>
#endif

#define LOCKDOCK_PREFERRED_UUID_KEY CFSTR("preferredDisplayUUID")
#define LOCKDOCK_PREFERRED_BUILTIN_KEY CFSTR("preferredDisplayBuiltin")
#define LOCKDOCK_PREFERRED_VENDOR_KEY CFSTR("preferredDisplayVendor")
#define LOCKDOCK_PREFERRED_MODEL_KEY CFSTR("preferredDisplayModel")
#define LOCKDOCK_PREFERRED_SERIAL_KEY CFSTR("preferredDisplaySerial")
#define LOCKDOCK_PREFERENCES_DOMAIN CFSTR(LOCKDOCK_BUNDLE_ID)
#ifdef LOCKDOCK_TESTING
#define LOCKDOCK_TEST_PREFERENCES_DOMAIN_ENV "LOCKDOCK_TEST_PREFERENCES_DOMAIN"

typedef struct {
    bool has_value;
    char domain[128];
    LockDockDisplayIdentity identity;
} LockDockTestPreferencesStore;

static LockDockTestPreferencesStore g_lockdock_test_preferences_store = {0};
#endif

static void lockdock_set_error(char *buffer,
                               size_t buffer_size,
                               const char *message) {
    if (buffer == NULL || buffer_size == 0) {
        return;
    }

    snprintf(buffer, buffer_size, "%s", message);
}

static CFStringRef lockdock_preferences_copy_domain(void) {
#ifdef LOCKDOCK_TESTING
    const char *override_domain = getenv(LOCKDOCK_TEST_PREFERENCES_DOMAIN_ENV);

    if (override_domain != NULL && override_domain[0] != '\0') {
        return CFStringCreateWithCString(kCFAllocatorDefault, override_domain,
                                         kCFStringEncodingUTF8);
    }
#endif

    return CFRetain(LOCKDOCK_PREFERENCES_DOMAIN);
}

#ifdef LOCKDOCK_TESTING
static bool lockdock_preferences_use_test_store(void) {
    const char *override_domain = getenv(LOCKDOCK_TEST_PREFERENCES_DOMAIN_ENV);

    if (override_domain == NULL || override_domain[0] == '\0') {
        memset(&g_lockdock_test_preferences_store, 0,
               sizeof(g_lockdock_test_preferences_store));
        return false;
    }

    if (strcmp(g_lockdock_test_preferences_store.domain, override_domain) != 0) {
        memset(&g_lockdock_test_preferences_store, 0,
               sizeof(g_lockdock_test_preferences_store));
        snprintf(g_lockdock_test_preferences_store.domain,
                 sizeof(g_lockdock_test_preferences_store.domain), "%s",
                 override_domain);
    }

    return true;
}
#endif

static bool lockdock_preferences_sync(char *error, size_t error_size) {
#ifdef LOCKDOCK_TESTING
    if (lockdock_preferences_use_test_store()) {
        (void)error;
        (void)error_size;
        return true;
    }
#endif

    CFStringRef domain = lockdock_preferences_copy_domain();
    bool synchronized;

    if (domain == NULL) {
        lockdock_set_error(error, error_size, "Failed to prepare preferences");
        return false;
    }

    synchronized = CFPreferencesAppSynchronize(domain);
    CFRelease(domain);

    if (synchronized) {
        return true;
    }

    lockdock_set_error(error, error_size, "Failed to synchronize preferences");
    return false;
}

static void lockdock_preferences_set_uint32(CFStringRef key, uint32_t value) {
    int64_t signed_value = (int64_t)value;
    CFNumberRef number =
        CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &signed_value);
    CFStringRef domain = lockdock_preferences_copy_domain();

    if (domain == NULL) {
        return;
    }

    if (number == NULL) {
        CFPreferencesSetAppValue(key, NULL, domain);
        CFRelease(domain);
        return;
    }

    CFPreferencesSetAppValue(key, number, domain);
    CFRelease(number);
    CFRelease(domain);
}

static bool lockdock_preferences_copy_uint32(CFStringRef key, uint32_t *value_out) {
    CFPropertyListRef value = NULL;
    int64_t signed_value = 0;
    CFStringRef domain;

    if (value_out == NULL) {
        return false;
    }

    domain = lockdock_preferences_copy_domain();
    if (domain == NULL) {
        return false;
    }

    value = CFPreferencesCopyAppValue(key, domain);
    CFRelease(domain);
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

static bool lockdock_preferences_copy_bool(CFStringRef key, bool *value_out) {
    CFPropertyListRef value = NULL;
    CFStringRef domain;

    if (value_out == NULL) {
        return false;
    }

    domain = lockdock_preferences_copy_domain();
    if (domain == NULL) {
        return false;
    }

    value = CFPreferencesCopyAppValue(key, domain);
    CFRelease(domain);
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

bool lockdock_preferences_save_preferred_display(
    const LockDockDisplayIdentity *identity,
    char *error,
    size_t error_size) {
    CFStringRef uuid = NULL;
    CFStringRef domain = NULL;

    if (!lockdock_display_identity_is_valid(identity)) {
        lockdock_set_error(error, error_size, "Internal error");
        return false;
    }

#ifdef LOCKDOCK_TESTING
    if (lockdock_preferences_use_test_store()) {
        g_lockdock_test_preferences_store.identity = *identity;
        g_lockdock_test_preferences_store.has_value = true;
        return true;
    }
#endif

    domain = lockdock_preferences_copy_domain();
    if (domain == NULL) {
        lockdock_set_error(error, error_size, "Failed to prepare preferences");
        return false;
    }

    if (identity->uuid[0] != '\0') {
        uuid = CFStringCreateWithCString(kCFAllocatorDefault, identity->uuid,
                                         kCFStringEncodingUTF8);
        if (uuid == NULL) {
            CFRelease(domain);
            lockdock_set_error(error, error_size, "Failed to encode display UUID");
            return false;
        }
    }

    CFPreferencesSetAppValue(LOCKDOCK_PREFERRED_UUID_KEY, uuid, domain);
    CFPreferencesSetAppValue(LOCKDOCK_PREFERRED_BUILTIN_KEY,
                             identity->is_builtin ? kCFBooleanTrue : kCFBooleanFalse,
                             domain);
    lockdock_preferences_set_uint32(LOCKDOCK_PREFERRED_VENDOR_KEY,
                                    identity->vendor_number);
    lockdock_preferences_set_uint32(LOCKDOCK_PREFERRED_MODEL_KEY,
                                    identity->model_number);
    lockdock_preferences_set_uint32(LOCKDOCK_PREFERRED_SERIAL_KEY,
                                    identity->serial_number);

    if (uuid != NULL) {
        CFRelease(uuid);
    }
    CFRelease(domain);

    return lockdock_preferences_sync(error, error_size);
}

bool lockdock_preferences_load_preferred_display(
    LockDockDisplayIdentity *identity_out) {
    CFPropertyListRef uuid_value = NULL;
    bool has_builtin = false;
    CFStringRef domain;

    if (identity_out == NULL) {
        return false;
    }

    memset(identity_out, 0, sizeof(*identity_out));

#ifdef LOCKDOCK_TESTING
    if (lockdock_preferences_use_test_store()) {
        if (!g_lockdock_test_preferences_store.has_value) {
            return false;
        }

        *identity_out = g_lockdock_test_preferences_store.identity;
        return lockdock_display_identity_is_valid(identity_out);
    }
#endif

    domain = lockdock_preferences_copy_domain();
    if (domain == NULL) {
        return false;
    }

    uuid_value = CFPreferencesCopyAppValue(LOCKDOCK_PREFERRED_UUID_KEY, domain);
    CFRelease(domain);
    if (uuid_value != NULL) {
        if (CFGetTypeID(uuid_value) == CFStringGetTypeID()) {
            CFStringGetCString((CFStringRef)uuid_value, identity_out->uuid,
                               sizeof(identity_out->uuid), kCFStringEncodingUTF8);
        }

        CFRelease(uuid_value);
    }

    if (lockdock_preferences_copy_bool(LOCKDOCK_PREFERRED_BUILTIN_KEY,
                                       &identity_out->is_builtin)) {
        has_builtin = true;
    }

    lockdock_preferences_copy_uint32(LOCKDOCK_PREFERRED_VENDOR_KEY,
                                     &identity_out->vendor_number);
    lockdock_preferences_copy_uint32(LOCKDOCK_PREFERRED_MODEL_KEY,
                                     &identity_out->model_number);
    lockdock_preferences_copy_uint32(LOCKDOCK_PREFERRED_SERIAL_KEY,
                                     &identity_out->serial_number);

    if (!lockdock_display_identity_is_valid(identity_out)) {
        if (!has_builtin) {
            return false;
        }

        return identity_out->is_builtin;
    }

    return true;
}

bool lockdock_preferences_clear_preferred_display(char *error, size_t error_size) {
#ifdef LOCKDOCK_TESTING
    if (lockdock_preferences_use_test_store()) {
        memset(&g_lockdock_test_preferences_store.identity, 0,
               sizeof(g_lockdock_test_preferences_store.identity));
        g_lockdock_test_preferences_store.has_value = false;
        return true;
    }
#endif

    CFStringRef domain = lockdock_preferences_copy_domain();

    if (domain == NULL) {
        lockdock_set_error(error, error_size, "Failed to prepare preferences");
        return false;
    }

    CFPreferencesSetAppValue(LOCKDOCK_PREFERRED_UUID_KEY, NULL, domain);
    CFPreferencesSetAppValue(LOCKDOCK_PREFERRED_BUILTIN_KEY, NULL, domain);
    CFPreferencesSetAppValue(LOCKDOCK_PREFERRED_VENDOR_KEY, NULL, domain);
    CFPreferencesSetAppValue(LOCKDOCK_PREFERRED_MODEL_KEY, NULL, domain);
    CFPreferencesSetAppValue(LOCKDOCK_PREFERRED_SERIAL_KEY, NULL, domain);
    CFRelease(domain);
    return lockdock_preferences_sync(error, error_size);
}
