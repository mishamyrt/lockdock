#ifndef LOCKDOCKD_DISPLAY_H
#define LOCKDOCKD_DISPLAY_H

#include <CoreGraphics/CoreGraphics.h>

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

CGDirectDisplayID lockdockd_resolve_display_arg(const char *arg);
LockDockdSafeSegment lockdockd_find_safe_edge_segment(CGDirectDisplayID target_id,
                                                      LockDockdDockOrientation edge);
CGDirectDisplayID lockdockd_find_display_at_point(CGPoint point);

#endif
