#ifndef lockdock_DISPLAY_H
#define lockdock_DISPLAY_H

#include <CoreGraphics/CoreGraphics.h>
#include <stdbool.h>
#include <stdint.h>

#define lockdock_MAX_DISPLAYS 32
#define lockdock_DISPLAY_UUID_BUFFER_SIZE 64

typedef struct {
    bool is_builtin;
    uint32_t vendor_number;
    uint32_t model_number;
    uint32_t serial_number;
    char uuid[lockdock_DISPLAY_UUID_BUFFER_SIZE];
} LockDockDisplayIdentity;

typedef struct {
    CGFloat start;
    CGFloat end;
    CGFloat width;
    CGFloat center;
} LockDockSafeSegment;

typedef enum : uint8_t {
    lockdock_ORIENT_BOTTOM = 0,
    lockdock_ORIENT_LEFT = 1,
    lockdock_ORIENT_RIGHT = 2,
} LockDockDockOrientation;

uint32_t lockdock_get_active_displays(CGDirectDisplayID *displays,
                                      uint32_t max_displays);
int lockdock_find_display_index(CGDirectDisplayID display_id);
bool lockdock_display_identity_is_valid(const LockDockDisplayIdentity *identity);
bool lockdock_copy_display_identity(CGDirectDisplayID display_id,
                                    LockDockDisplayIdentity *identity_out);
bool lockdock_find_active_display_by_identity(
    const LockDockDisplayIdentity *identity,
    CGDirectDisplayID *display_id_out);
LockDockSafeSegment lockdock_find_safe_edge_segment(CGDirectDisplayID target_id,
                                                    LockDockDockOrientation edge);
bool lockdock_edge_point_has_contact(CGDirectDisplayID target_id,
                                     LockDockDockOrientation edge,
                                     CGFloat point_along_edge);
CGDirectDisplayID lockdock_find_display_at_point(CGPoint point);

#endif
