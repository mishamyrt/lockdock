#ifndef LOCKDOCK_MOUSE_H
#define LOCKDOCK_MOUSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    double x;
    double y;
} LockDockMousePoint;

bool lockdock_mouse_copy_location(LockDockMousePoint *point_out);
void *lockdock_mouse_event_source_create(void);
void lockdock_mouse_event_source_set_suppression_interval(
    void *source,
    double interval
);
void lockdock_mouse_release(void *object);
void lockdock_mouse_warp(LockDockMousePoint point);
void lockdock_mouse_post_moved(void *source, LockDockMousePoint point);
void lockdock_mouse_post_delta(
    void *source,
    LockDockMousePoint point,
    int64_t delta_x,
    int64_t delta_y
);
bool lockdock_mouse_start_event_tap(char *error, size_t error_size);
void lockdock_mouse_stop_event_tap(void);

extern bool lockdock_mouse_should_suppress_event(double x, double y);

#endif
