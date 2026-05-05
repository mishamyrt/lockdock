#include "lockdockd_display.h"

#include <ColorSync/ColorSyncDevice.h>
#include <CoreFoundation/CoreFoundation.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define LOCKDOCKD_MAX_OVERLAPS 64

typedef struct {
    CGFloat start;
    CGFloat end;
} LockDockdOverlap;

typedef bool (*LockDockdActiveDisplayVisitor)(CGDirectDisplayID display_id,
                                              uint32_t index,
                                              void *context);

typedef struct {
    const LockDockdDisplayIdentity *identity;
    CGDirectDisplayID match;
    CGDirectDisplayID fallback_match;
    bool found_match;
    bool found_fallback_match;
} LockDockdIdentitySearchContext;

typedef struct {
    CGDirectDisplayID target_id;
    LockDockdDockOrientation edge;
    CGFloat edge_min;
    CGFloat edge_max;
    CGFloat edge_cross_pos;
    LockDockdOverlap *overlaps;
    int overlap_count;
} LockDockdOverlapCollectionContext;

typedef struct {
    CGDirectDisplayID display_id;
    int index;
} LockDockdDisplayIndexContext;

typedef struct {
    CGPoint point;
    CGDirectDisplayID display_id;
} LockDockdPointSearchContext;

bool lockdockd_display_identity_is_valid(const LockDockdDisplayIdentity *identity) {
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

static void lockdockd_for_each_active_display(LockDockdActiveDisplayVisitor visitor,
                                              void *context) {
    CGDirectDisplayID displays[LOCKDOCKD_MAX_DISPLAYS];
    uint32_t count = 0;

    if (visitor == NULL) {
        return;
    }

    count = lockdockd_get_active_displays(displays, LOCKDOCKD_MAX_DISPLAYS);

    for (uint32_t i = 0; i < count; i++) {
        if (!visitor(displays[i], i, context)) {
            break;
        }
    }
}

static int lockdockd_compare_overlaps(const void *left_ptr, const void *right_ptr) {
    const LockDockdOverlap *left = left_ptr;
    const LockDockdOverlap *right = right_ptr;

    if (left->start < right->start) {
        return -1;
    }

    if (left->start > right->start) {
        return 1;
    }

    if (left->end < right->end) {
        return -1;
    }

    if (left->end > right->end) {
        return 1;
    }

    return 0;
}

static void lockdockd_update_best_safe_segment(LockDockdSafeSegment *best,
                                               CGFloat start,
                                               CGFloat end) {
    CGFloat width;

    if (best == NULL || end <= start) {
        return;
    }

    width = end - start;

    if (width > best->width) {
        best->start = start;
        best->end = end;
        best->width = width;
        best->center = start + width / 2.0;
    }
}

static bool lockdockd_find_display_identity_match(CGDirectDisplayID display_id,
                                                  uint32_t index,
                                                  void *context) {
    LockDockdIdentitySearchContext *search = context;
    LockDockdDisplayIdentity current_identity;

    (void)index;

    if (search == NULL || search->identity == NULL) {
        return false;
    }

    if (!lockdockd_copy_display_identity(display_id, &current_identity)) {
        return true;
    }

    if (search->identity->uuid[0] != '\0' && current_identity.uuid[0] != '\0' &&
        strcmp(current_identity.uuid, search->identity->uuid) == 0) {
        search->match = display_id;
        search->found_match = true;
        return false;
    }

    if (!search->found_fallback_match && lockdockd_display_identity_fallback_matches(
                                             &current_identity, search->identity)) {
        search->fallback_match = display_id;
        search->found_fallback_match = true;
        return search->identity->uuid[0] != '\0';
    }

    return true;
}

static bool lockdockd_collect_display_overlap(CGDirectDisplayID display_id,
                                              uint32_t index,
                                              void *context) {
    LockDockdOverlapCollectionContext *collection = context;
    CGRect other;
    CGFloat other_min_along;
    CGFloat other_max_along;
    CGFloat other_min_cross;
    CGFloat other_max_cross;
    CGFloat overlap_start;
    CGFloat overlap_end;

    (void)index;

    if (collection == NULL) {
        return false;
    }

    if (display_id == collection->target_id) {
        return true;
    }

    other = CGDisplayBounds(display_id);

    if (collection->edge == LOCKDOCKD_ORIENT_BOTTOM) {
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

    if (other_max_cross < collection->edge_cross_pos - 1.0 ||
        other_min_cross > collection->edge_cross_pos + 1.0) {
        return true;
    }

    overlap_start = fmax(collection->edge_min, other_min_along);
    overlap_end = fmin(collection->edge_max, other_max_along);

    if (overlap_end > overlap_start && (overlap_end - overlap_start) > 2.0) {
        collection->overlaps[collection->overlap_count].start = overlap_start;
        collection->overlaps[collection->overlap_count].end = overlap_end;
        collection->overlap_count++;
    }

    return collection->overlap_count < LOCKDOCKD_MAX_OVERLAPS;
}

static bool lockdockd_find_display_index_match(CGDirectDisplayID display_id,
                                               uint32_t index,
                                               void *context) {
    LockDockdDisplayIndexContext *search = context;

    if (search == NULL) {
        return false;
    }

    if (display_id != search->display_id) {
        return true;
    }

    search->index = (int)index;
    return false;
}

static bool lockdockd_find_display_at_point_match(CGDirectDisplayID display_id,
                                                  uint32_t index,
                                                  void *context) {
    LockDockdPointSearchContext *search = context;
    CGRect bounds;

    (void)index;

    if (search == NULL) {
        return false;
    }

    bounds = CGDisplayBounds(display_id);

    if (search->point.x < bounds.origin.x ||
        search->point.x >= bounds.origin.x + bounds.size.width ||
        search->point.y < bounds.origin.y ||
        search->point.y >= bounds.origin.y + bounds.size.height) {
        return true;
    }

    search->display_id = display_id;
    return false;
}

bool lockdockd_find_active_display_by_identity(
    const LockDockdDisplayIdentity *identity,
    CGDirectDisplayID *display_id_out) {
    LockDockdIdentitySearchContext search = {0};

    if (!lockdockd_display_identity_is_valid(identity) || display_id_out == NULL) {
        return false;
    }

    search.identity = identity;
    lockdockd_for_each_active_display(lockdockd_find_display_identity_match,
                                      &search);

    if (search.found_match) {
        *display_id_out = search.match;
        return true;
    }

    if (search.found_fallback_match) {
        *display_id_out = search.fallback_match;
        return true;
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
    LockDockdDisplayIndexContext search = {0};

    search.display_id = display_id;
    search.index = -1;

    lockdockd_for_each_active_display(lockdockd_find_display_index_match, &search);

    return search.index;
}

LockDockdSafeSegment lockdockd_find_safe_edge_segment(
    CGDirectDisplayID target_id,
    LockDockdDockOrientation edge) {
    CGRect target = CGDisplayBounds(target_id);
    CGFloat edge_min;
    CGFloat edge_max;
    CGFloat edge_cross_pos;
    LockDockdOverlap overlaps[LOCKDOCKD_MAX_OVERLAPS];
    LockDockdOverlapCollectionContext overlap_collection = {0};
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

    overlap_collection.target_id = target_id;
    overlap_collection.edge = edge;
    overlap_collection.edge_min = edge_min;
    overlap_collection.edge_max = edge_max;
    overlap_collection.edge_cross_pos = edge_cross_pos;
    overlap_collection.overlaps = overlaps;

    lockdockd_for_each_active_display(lockdockd_collect_display_overlap,
                                      &overlap_collection);

    pos = edge_min;

    if (overlap_collection.overlap_count > 0) {
        LockDockdOverlap current;

        qsort(overlaps, (size_t)overlap_collection.overlap_count,
              sizeof(overlaps[0]), lockdockd_compare_overlaps);

        current = overlaps[0];

        for (int i = 1; i < overlap_collection.overlap_count; i++) {
            if (overlaps[i].start <= current.end) {
                current.end = fmax(current.end, overlaps[i].end);
                continue;
            }

            lockdockd_update_best_safe_segment(&best, pos, current.start);
            pos = fmax(pos, current.end);
            current = overlaps[i];
        }

        lockdockd_update_best_safe_segment(&best, pos, current.start);
        pos = fmax(pos, current.end);
    }

    lockdockd_update_best_safe_segment(&best, pos, edge_max);

    if (best.width <= 0) {
        best.center = edge_min + (edge_max - edge_min) / 2.0;
        best.width = edge_max - edge_min;
    }

    return best;
}

CGDirectDisplayID lockdockd_find_display_at_point(CGPoint point) {
    LockDockdPointSearchContext search = {0};

    search.point = point;

    lockdockd_for_each_active_display(lockdockd_find_display_at_point_match,
                                      &search);

    return search.display_id;
}
