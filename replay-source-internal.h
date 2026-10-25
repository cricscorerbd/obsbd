/*
 * replay-source-internal.h
 *
 * Internal header shared between replay-source.c and speed-popup.c.
 * Exposes struct replay_source and the small set of functions that the
 * speed popup needs to call back into the replay engine.
 *
 * DO NOT include this from any public/external header.
 */

#pragma once

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#if LIBOBS_API_VER >= MAKE_SEMANTIC_VERSION(30, 1, 0)
#include <util/deque.h>
#else
#include <util/circlebuf.h>
#endif
#include <media-io/video-io.h>
#include <media-io/video-frame.h>
#include <media-io/video-scaler.h>
#include <media-io/audio-io.h>
#include "replay.h"

/* ------------------------------------------------------------------ */
/*  Types used inside struct replay_source                            */
/* ------------------------------------------------------------------ */

enum saving_status {
	SAVING_STATUS_NONE      = 0,
	SAVING_STATUS_STARTING  = 1,
	SAVING_STATUS_STARTING2 = 2,
	SAVING_STATUS_SAVING    = 3,
	SAVING_STATUS_STOPPING  = 4
};

struct replay {
	struct obs_source_frame **video_frames;
	uint64_t video_frame_count;
	struct obs_audio_data *audio_frames;
	struct audio_convert_info oai;
	uint64_t audio_frame_count;
	uint64_t first_frame_timestamp;
	uint64_t last_frame_timestamp;
	uint64_t duration;
	int64_t  trim_front;
	int64_t  trim_end;
};

/* ------------------------------------------------------------------ */
/*  replay_source – the main plugin context struct                    */
/* ------------------------------------------------------------------ */
struct replay_source {
	obs_source_t *source;
	obs_source_t *source_filter;
	obs_source_t *source_audio_filter;
	char *source_name;
	char *source_audio_name;
	float speed_percent;
	bool  backward;
	bool  backward_start;
	int   visibility_action;
	int   end_action;
	char *next_scene_name;
	bool  next_scene_disabled;
	char *load_switch_scene_name;
	obs_hotkey_id replay_hotkey;
	obs_hotkey_id next_hotkey;
	obs_hotkey_id previous_hotkey;
	obs_hotkey_id first_hotkey;
	obs_hotkey_id last_hotkey;
	obs_hotkey_id remove_hotkey;
	obs_hotkey_id clear_hotkey;
	obs_hotkey_id restart_hotkey;
	obs_hotkey_id pause_hotkey;
	obs_hotkey_id faster_hotkey;
	obs_hotkey_id slower_hotkey;
	obs_hotkey_id faster_by_5_hotkey;
	obs_hotkey_id slower_by_5_hotkey;
	obs_hotkey_id normal_or_faster_hotkey;
	obs_hotkey_id normal_or_slower_hotkey;
	obs_hotkey_id normal_speed_hotkey;
	obs_hotkey_id half_speed_hotkey;
	obs_hotkey_id double_speed_hotkey;
	obs_hotkey_id trim_front_hotkey;
	obs_hotkey_id trim_end_hotkey;
	obs_hotkey_id trim_reset_hotkey;
	obs_hotkey_id reverse_hotkey;
	obs_hotkey_id forward_hotkey;
	obs_hotkey_id backward_hotkey;
	obs_hotkey_id forward_or_faster_hotkey;
	obs_hotkey_id backward_or_faster_hotkey;
	obs_hotkey_id save_hotkey;
	obs_hotkey_id enable_hotkey;
	obs_hotkey_id disable_hotkey;
	obs_hotkey_id enable_next_scene_hotkey;
	obs_hotkey_id disable_next_scene_hotkey;
	obs_hotkey_id next_scene_current_hotkey;
	obs_hotkey_id next_scene_hotkey;
	obs_hotkey_id next_frame_hotkey;
	obs_hotkey_id prev_frame_hotkey;
	obs_hotkey_id next_n_frames_hotkey;
	obs_hotkey_id prev_n_frames_hotkey;
	int64_t  start_timestamp;
	uint64_t previous_frame_timestamp;
	uint64_t pause_timestamp;
	int64_t  start_delay;
	int64_t  retrieve_delay;
	int64_t  frame_step_count;
	uint64_t retrieve_timestamp;
	uint64_t threshold_timestamp;
	struct obs_source_audio audio;

	bool disabled;
	bool play;
	bool restart;
	bool active;
	bool end;
	bool stepped;
	enum saving_status saving_status;

	int replay_position;
	int replay_max;
#if LIBOBS_API_VER >= MAKE_SEMANTIC_VERSION(30, 1, 0)
	struct deque replays;
#else
	struct circlebuf replays;
#endif
	struct replay current_replay;
	struct replay saving_replay;

	uint64_t video_frame_position;
	uint64_t video_save_position;
	bool speed_popup_always_show;

	uint64_t audio_frame_position;
	struct obs_audio_data audio_output;

	pthread_mutex_t video_mutex;
	pthread_mutex_t audio_mutex;
	pthread_mutex_t replay_mutex;

	uint32_t known_width;
	uint32_t known_height;

	video_t       *video_output;
	obs_output_t  *fileOutput;
	obs_encoder_t *h264Recording;
	audio_t       *audio_t;
	video_scaler_t *scaler;
	bool  lossless;
	bool  auto_save;
	char *file_format;
	char *directory;
	uint64_t       start_save_timestamp;
	obs_encoder_t *aac;
	char *progress_source_name;
	char *text_source_name;
	char *text_format;
	bool sound_trigger;
	bool filter_loaded;
	bool free_after_save;
	void *speed_popup;
	struct ball_tracker *ball_tracker;

	float zoom_percent;
	video_scaler_t *zoom_scaler;
	uint32_t zoom_scaler_crop_w;
	uint32_t zoom_scaler_crop_h;
	enum video_format zoom_scaler_format;
	struct obs_source_frame *zoomed_frame;
};

/* ------------------------------------------------------------------ */
/*  Functions in replay-source.c that speed-popup.c may call          */
/* ------------------------------------------------------------------ */

/* Update playback speed and notify the OBS settings system. */
void update_speed(struct replay_source *context, float new_speed);

/* Switch direction to forward (clears the backward flag correctly). */
void replay_set_forward(struct replay_source *context);

/* Switch direction to backward (sets the backward flag correctly). */
void replay_set_backward(struct replay_source *context);

/* Toggle play / pause state. */
void replay_toggle_play_pause(struct replay_source *context);

/* Step frame backward/forward. */
void replay_step_frames(void *data, bool pressed, bool forward, uint64_t num_frames);

/* Update zoom percentage (100.0f = 1.0x up to 300.0f = 3.0x). */
void update_zoom(struct replay_source *context, float new_zoom);

/* Reset zoom back to 100% (1.0x - normal). */
void replay_reset_zoom(struct replay_source *context);

/* Obtain zoomed frame if zoom > 100%, else returns src. */
struct obs_source_frame *replay_get_zoomed_frame(struct replay_source *context, struct obs_source_frame *src);



