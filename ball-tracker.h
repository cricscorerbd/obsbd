#pragma once
#include <obs-module.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum ball_tracker_color {
	BALL_TRACKER_COLOR_MAGENTA = 0,
	BALL_TRACKER_COLOR_RED = 1,
	BALL_TRACKER_COLOR_YELLOW = 2,
	BALL_TRACKER_COLOR_BLUE = 3
};
#define BALL_TRACKER_COLOR_COUNT 4

struct ball_point {
	float u; /* 0.0f .. 1.0f */
	float v; /* 0.0f .. 1.0f */
	bool  set;
};

struct ball_tracker;
struct replay_source;

struct ball_tracker *ball_tracker_create(struct replay_source *context);
void ball_tracker_destroy(struct ball_tracker *bt);

void ball_tracker_show(struct ball_tracker *bt);
void ball_tracker_hide(struct ball_tracker *bt);
void ball_tracker_reset_points(struct ball_tracker *bt);
void ball_tracker_toggle_visibility(struct ball_tracker *bt);
void ball_tracker_cycle_color(struct ball_tracker *bt);
void ball_tracker_notify_frame(struct ball_tracker *bt);

/* Renders ball trajectory overlay onto frame if active and enabled.
 * If inactive/disabled, returns frame immediately with ZERO overhead.
 * Guarantees that raw stored replay buffer frames are NEVER modified. */
struct obs_source_frame *ball_tracker_render(struct ball_tracker *bt, struct obs_source_frame *frame);

#ifdef __cplusplus
}
#endif
