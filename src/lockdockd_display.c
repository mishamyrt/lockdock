#include "lockdockd_display.h"

#include <ColorSync/ColorSyncDevice.h>
#include <CoreFoundation/CoreFoundation.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>

#define LOCKDOCKD_MAX_OVERLAPS 64

typedef struct {
    CGFloat start;
    CGFloat end;
} LockDockdOverlap;

bool lockdockd_display_identity_is_valid(
    const LockDockdDisplayIdentity *identity) {
    if (identity == NULL) {
        return false;
    }

    return identity->uuid[0] != '\0' || identity->is_builtin ||
           identity->vendor_number != 0 || identity->model_number != 0 ||
           identity->serial_number != 0;
}

bool lockdockd_copy_display_identity(CGDirectDisplayID display_id,
                                     LockDockdDisplayIdentity *identity_out) {
    CFUUIDRef uuid = NULL;
    CFStringRef uuid_string = NULL;

    if (display_id == 0 || identity_out == NULL) {
        return false;
    }

    memset(identity_out, 0, sizeof(*identity_out));
    identity_out->is_builtin = CGDisplayIsBuiltin(display_id) != 0;
    identity_out->vendor_number = CGDisplayVendorNumber(display_id);
    identity_out->model_number = CGDisplayModelNumber(display_id);
    identity_out->serial_number = CGDisplaySerialNumber(display_id);

    uuid = CGDisplayCreateUUIDFromDisplayID(display_id);
    if (uuid != NULL) {
        uuid_string = CFUUIDCreateString(kCFAllocatorDefault, uuid);
        if (uuid_string != NULL) {
            CFStringGetCString(uuid_string, identity_out->uuid,
                               sizeof(identity_out->uuid), kCFStringEncodingUTF8);
            CFRelease(uuid_string);
        }

        CFRelease(uuid);
    }

    return lockdockd_display_identity_is_valid(identity_out);
}

static bool lockdockd_display_identity_fallback_matches(
    const LockDockdDisplayIdentity *left,
    const LockDockdDisplayIdentity *right) {
    if (left == NULL || right == NULL) {
        return false;
    }

    return left->is_builtin == right->is_builtin &&
           left->vendor_number == right->vendor_number &&
           left->model_number == right->model_number &&
           left->serial_number == right->serial_number;
}

bool lockdockd_find_active_display_by_identity(
    const LockDockdDisplayIdentity *identity,
    CGDirectDisplayID *display_id_out) {
    CGDirectDisplayID displays[LOCKDOCKD_MAX_DISPLAYS];
    uint32_t count = 0;

    if (!lockdockd_display_identity_is_valid(identity) || display_id_out == NULL) {
        return false;
    }

    count = lockdockd_get_active_displays(displays, LOCKDOCKD_MAX_DISPLAYS);

    if (identity->uuid[0] != '\0') {
        for (uint32_t i = 0; i < count; i++) {
            LockDockdDisplayIdentity current_identity;

            if (!lockdockd_copy_display_identity(displays[i], &current_identity)) {
                continue;
            }

            if (current_identity.uuid[0] != '\0' &&
                strcmp(current_identity.uuid, identity->uuid) == 0) {
                *display_id_out = displays[i];
                return true;
            }
        }
    }

    for (uint32_t i = 0; i < count; i++) {
        LockDockdDisplayIdentity current_identity;

        if (!lockdockd_copy_display_identity(displays[i], &current_identity)) {
            continue;
        }

        if (lockdockd_display_identity_fallback_matches(&current_identity,
                                                        identity)) {
            *display_id_out = displays[i];
            return true;
        }
    }

    return false;
}

uint32_t lockdockd_get_active_displays(CGDirectDisplayID *displays,
                                       uint32_t max_displays) {
    uint32_t count = 0;

    if (displays == NULL || max_displays == 0) {
        return 0;
    }

    CGGetActiveDisplayList(max_displays, displays, &count);
    return count;
}

int lockdockd_find_display_index(CGDirectDisplayID display_id) {
    CGDirectDisplayID displays[LOCKDOCKD_MAX_DISPLAYS];
    uint32_t count = lockdockd_get_active_displays(displays, LOCKDOCKD_MAX_DISPLAYS);

    for (uint32_t i = 0; i < count; i++) {
        if (displays[i] == display_id) {
            return (int)i;
        }
    }

    return -1;
}

LockDockdSafeSegment lockdockd_find_safe_edge_segment(
    CGDirectDisplayID target_id,
    LockDockdDockOrientation edge) {
    CGDirectDisplayID displays[LOCKDOCKD_MAX_DISPLAYS];
    uint32_t count = lockdockd_get_active_displays(displays, LOCKDOCKD_MAX_DISPLAYS);
    CGRect target = CGDisplayBounds(target_id);
    CGFloat edge_min;
    CGFloat edge_max;
    CGFloat edge_cross_pos;
    LockDockdOverlap overlaps[LOCKDOCKD_MAX_OVERLAPS];
    int overlap_count = 0;
    LockDockdSafeSegment best = {0, 0, 0, 0};
    CGFloat pos;

    if (edge == LOCKDOCKD_ORIENT_BOTTOM) {
        edge_min = target.origin.x;
        edge_max = target.origin.x + target.size.width;
        edge_cross_pos = target.origin.y + target.size.height;
    } else if (edge == LOCKDOCKD_ORIENT_LEFT) {
        edge_min = target.origin.y;
        edge_max = target.origin.y + target.size.height;
        edge_cross_pos = target.origin.x;
    } else {
        edge_min = target.origin.y;
        edge_max = target.origin.y + target.size.height;
        edge_cross_pos = target.origin.x + target.size.width;
    }

    for (uint32_t i = 0; i < count; i++) {
        CGRect other;
        CGFloat other_min_along;
        CGFloat other_max_along;
        CGFloat other_min_cross;
        CGFloat other_max_cross;
        CGFloat overlap_start;
        CGFloat overlap_end;

        if (displays[i] == target_id) {
            continue;
        }

        other = CGDisplayBounds(displays[i]);

        if (edge == LOCKDOCKD_ORIENT_BOTTOM) {
            other_min_along = other.origin.x;
            other_max_along = other.origin.x + other.size.width;
            other_min_cross = other.origin.y;
            other_max_cross = other.origin.y + other.size.height;
        } else {
            other_min_along = other.origin.y;
            other_max_along = other.origin.y + other.size.height;
            other_min_cross = other.origin.x;
            other_max_cross = other.origin.x + other.size.width;
        }

        if (other_max_cross < edge_cross_pos - 1.0 ||
            other_min_cross > edge_cross_pos + 1.0) {
            continue;
        }

        overlap_start = fmax(edge_min, other_min_along);
        overlap_end = fmin(edge_max, other_max_along);

        if (overlap_end > overlap_start && (overlap_end - overlap_start) > 2.0) {
            overlaps[overlap_count].start = overlap_start;
            overlaps[overlap_count].end = overlap_end;
            overlap_count++;

            if (overlap_count >= LOCKDOCKD_MAX_OVERLAPS) {
                break;
            }
        }
    }

    for (int i = 0; i < overlap_count; i++) {
        for (int j = i + 1; j < overlap_count; j++) {
            if (overlaps[i].start > overlaps[j].start) {
                LockDockdOverlap tmp = overlaps[i];
                overlaps[i] = overlaps[j];
                overlaps[j] = tmp;
            }
        }
    }

    if (overlap_count > 0) {
        LockDockdOverlap merged[LOCKDOCKD_MAX_OVERLAPS];
        int merged_count = 0;

        for (int i = 0; i < overlap_count; i++) {
            if (merged_count == 0) {
                merged[0] = overlaps[i];
                merged_count = 1;
                continue;
            }

            if (overlaps[i].start <= merged[merged_count - 1].end) {
                merged[merged_count - 1].end =
                    fmax(merged[merged_count - 1].end, overlaps[i].end);
            } else {
                merged[merged_count] = overlaps[i];
                merged_count++;
            }
        }

        pos = edge_min;

        for (int i = 0; i < merged_count; i++) {
            CGFloat safe_start = pos;
            CGFloat safe_end = merged[i].start;

            if (safe_end > safe_start) {
                CGFloat width = safe_end - safe_start;

                if (width > best.width) {
                    best.start = safe_start;
                    best.end = safe_end;
                    best.width = width;
                    best.center = safe_start + width / 2.0;
                }
            }

            pos = fmax(pos, merged[i].end);
        }
    } else {
        pos = edge_min;
    }

    if (pos < edge_max) {
        CGFloat width = edge_max - pos;

        if (width > best.width) {
            best.start = pos;
            best.end = edge_max;
            best.width = width;
            best.center = pos + width / 2.0;
        }
    }

    if (best.width <= 0) {
        best.center = edge_min + (edge_max - edge_min) / 2.0;
        best.width = edge_max - edge_min;
    }

    return best;
}

CGDirectDisplayID lockdockd_find_display_at_point(CGPoint point) {
    CGDirectDisplayID displays[LOCKDOCKD_MAX_DISPLAYS];
    uint32_t count = lockdockd_get_active_displays(displays, LOCKDOCKD_MAX_DISPLAYS);

    for (uint32_t i = 0; i < count; i++) {
        CGRect bounds = CGDisplayBounds(displays[i]);

        if (point.x >= bounds.origin.x &&
            point.x < bounds.origin.x + bounds.size.width &&
            point.y >= bounds.origin.y &&
            point.y < bounds.origin.y + bounds.size.height) {
            return displays[i];
        }
    }

    return (CGDirectDisplayID)0;
}
