#pragma once

/* Forward declaration — full struct lives in speed-popup.c */
struct replay_source;

/* Create the popup window thread (called once per source instance). */
void speed_popup_create(struct replay_source *context);

/* Show the floating speed-meter window near the bottom-right of the screen. */
void speed_popup_show(struct replay_source *context);

/* Hide the window without destroying it. */
void speed_popup_hide(struct replay_source *context);

/* Push a speed/direction update to the window (non-blocking PostMessage). */
void speed_popup_notify(struct replay_source *context);

/* Destroy the window and free all resources. */
void speed_popup_destroy(struct replay_source *context);
