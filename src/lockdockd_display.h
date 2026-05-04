#ifndef LOCKDOCKD_DISPLAY_H
#define LOCKDOCKD_DISPLAY_H

#include <CoreGraphics/CoreGraphics.h>
#include <stdbool.h>
#include <stdint.h>

#define LOCKDOCKD_MAX_DISPLAYS 32
#define LOCKDOCKD_DISPLAY_UUID_BUFFER_SIZE 64

typedef struct {
    bool is_builtin;
    uint32_t vendor_number;
    uint32_t model_number;
    uint32_t serial_number;
    char uuid[LOCKDOCKD_DISPLAY_UUID_BUFFER_SIZE];
} LockDockdDisplayIdentity;

typedef struct {
    CGFloat start;
    CGFloat end;
    CGFloat width;
    CGFloat center;
} LockDockdSafeSegment;

typedef enum {
    LOCKDOCKD_ORIENT_BOTTOM = 0,
    LOCKDOCKD_ORIENT_LEFT = 1,
    LOCKDOCKD_ORIENT_RIGHT = 2,
} LockDockdDockOrientation;

uint32_t lockdockd_get_active_displays(CGDirectDisplayID *displays,
                                       uint32_t max_displays);
int lockdockd_find_display_index(CGDirectDisplayID display_id);
bool lockdockd_display_identity_is_valid(
    const LockDockdDisplayIdentity *identity);
bool lockdockd_copy_display_identity(CGDirectDisplayID display_id,
                                     LockDockdDisplayIdentity *identity_out);
bool lockdockd_find_active_display_by_identity(
    const LockDockdDisplayIdentity *identity,
    CGDirectDisplayID *display_id_out);
LockDockdSafeSegment lockdockd_find_safe_edge_segment(CGDirectDisplayID target_id,
                                                      LockDockdDockOrientation edge);
CGDirectDisplayID lockdockd_find_display_at_point(CGPoint point);

#endif
