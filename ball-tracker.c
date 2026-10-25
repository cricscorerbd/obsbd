#include "ball-tracker.h"
#include "replay-source-internal.h"
#include <util/platform.h>
#include <util/threading.h>
#include <media-io/video-scaler.h>
#include <media-io/video-frame.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <commctrl.h>

#define ID_BTN_PREV5       3001
#define ID_BTN_PREV1       3002
#define ID_BTN_PLAY_PAUSE  3003
#define ID_BTN_NEXT1       3004
#define ID_BTN_NEXT5       3005
#define ID_BTN_RESET       3010
#define ID_BTN_TOGGLE      3011
#define ID_BTN_COLOR       3012
#define ID_BTN_CLOSE       3013
#define ID_BTN_ANIMATE     3014

struct ball_tracker {
	struct replay_source *context;
	pthread_mutex_t mutex;

	/* 3-Point Calibration */
	struct ball_point points[3]; /* 0: Release, 1: Pitch, 2: Stumps */
	int point_count;             /* 0 to 3 */
	bool enabled;
	enum ball_tracker_color color;
	int thickness;               /* 4 to 12 */

	/* Trajectory Animation */
	bool is_animating;
	uint64_t anim_start_ns;
	uint64_t anim_duration_ns;

	/* Window geometry persistence */
	int saved_x;
	int saved_y;
	int saved_w;
	int saved_h;
	bool has_saved_pos;

	/* Dynamic preview canvas bounds */
	int preview_x;
	int preview_y;
	int preview_w;
	int preview_h;

	/* Output frame overlay buffer */
	struct obs_source_frame *overlay_frame;

	/* Win32 UI */
	HWND hwnd;
	HWND hwnd_status;
	HWND hwnd_btn_prev5;
	HWND hwnd_btn_prev1;
	HWND hwnd_btn_play_pause;
	HWND hwnd_btn_next1;
	HWND hwnd_btn_next5;
	HWND hwnd_btn_reset;
	HWND hwnd_btn_animate;
	HWND hwnd_btn_toggle;
	HWND hwnd_btn_color;
	HWND hwnd_btn_close;

	HFONT hfont_status;
	HFONT hfont_btns;
	HBRUSH hbr_bg;
	HBRUSH hbr_canvas_border;

	HANDLE thread;
	DWORD thread_id;
	bool is_showing;

	/* Preview video buffer */
	video_scaler_t *preview_scaler;
	uint32_t preview_scaler_w;
	uint32_t preview_scaler_h;
	uint32_t preview_scaler_target_w;
	uint32_t preview_scaler_target_h;
	enum video_format preview_scaler_format;
	uint8_t *preview_pixels;
	uint32_t preview_linesize;
};

static int s_bt_saved_x = 0;
static int s_bt_saved_y = 0;
static int s_bt_saved_w = 840;
static int s_bt_saved_h = 620;
static bool s_bt_has_saved_pos = false;

/* ------------------------------------------------------------------ */
/*  YUV / RGB Color Constants & Sets                                  */
/* ------------------------------------------------------------------ */
struct color_def {
	uint8_t r, g, b;
	uint8_t y, u, v;
};

struct tracker_color_set {
	struct color_def base;
	struct color_def highlight; /* Pastel tint for 3D glossy sheen */
	COLORREF gdi_base;
	COLORREF gdi_highlight;
	const wchar_t *name;
};

static const struct tracker_color_set s_color_sets[BALL_TRACKER_COLOR_COUNT] = {
	/* 0: Neon Magenta / Hot Pink (matching user reference picture) */
	{
		{255, 16, 160, 104, 160, 236},
		{255, 185, 235, 211, 142, 160},
		RGB(255, 16, 160),
		RGB(255, 185, 235),
		L"Color: Magenta"
	},
	/* 1: Cricket Ball Red */
	{
		{245, 35, 55, 80, 114, 245},
		{255, 180, 190, 200, 122, 165},
		RGB(245, 35, 55),
		RGB(255, 180, 190),
		L"Color: Red"
	},
	/* 2: Glowing Broadcast Gold / Yellow */
	{
		{255, 220, 20, 202, 26, 164},
		{255, 250, 180, 247, 90, 134},
		RGB(255, 220, 20),
		RGB(255, 250, 180),
		L"Color: Yellow"
	},
	/* 3: Electric Cyan / Sky Blue */
	{
		{20, 200, 255, 153, 185, 33},
		{180, 240, 255, 229, 142, 93},
		RGB(20, 200, 255),
		RGB(180, 240, 255),
		L"Color: Cyan"
	}
};

static const struct color_def s_white = {255, 255, 255, 235, 128, 128};

/* ------------------------------------------------------------------ */
/*  Pixel blending helpers                                            */
/* ------------------------------------------------------------------ */
static inline void blend_yuv(uint8_t *y_ptr, uint8_t *u_ptr, uint8_t *v_ptr,
                            uint8_t y_val, uint8_t u_val, uint8_t v_val, uint8_t a)
{
	uint32_t inv_a = 255 - a;
	if (y_ptr) *y_ptr = (uint8_t)((*y_ptr * inv_a + y_val * a) / 255);
	if (u_ptr) *u_ptr = (uint8_t)((*u_ptr * inv_a + u_val * a) / 255);
	if (v_ptr) *v_ptr = (uint8_t)((*v_ptr * inv_a + v_val * a) / 255);
}

static inline void blend_bgra(uint8_t *pix, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
	uint32_t inv_a = 255 - a;
	pix[0] = (uint8_t)((pix[0] * inv_a + b * a) / 255);
	pix[1] = (uint8_t)((pix[1] * inv_a + g * a) / 255);
	pix[2] = (uint8_t)((pix[2] * inv_a + r * a) / 255);
}

static void draw_point_pixel(struct obs_source_frame *f, int x, int y,
                            const struct color_def *c, uint8_t a)
{
	if (x < 0 || (uint32_t)x >= f->width || y < 0 || (uint32_t)y >= f->height)
		return;

	switch (f->format) {
	case VIDEO_FORMAT_NV12: {
		uint8_t *y_ptr = f->data[0] + y * f->linesize[0] + x;
		uint8_t *u_ptr = f->data[1] + (y / 2) * f->linesize[1] + (x / 2) * 2;
		uint8_t *v_ptr = u_ptr + 1;
		blend_yuv(y_ptr, u_ptr, v_ptr, c->y, c->u, c->v, a);
		break;
	}
	case VIDEO_FORMAT_I420: {
		uint8_t *y_ptr = f->data[0] + y * f->linesize[0] + x;
		uint8_t *u_ptr = f->data[1] + (y / 2) * f->linesize[1] + (x / 2);
		uint8_t *v_ptr = f->data[2] + (y / 2) * f->linesize[2] + (x / 2);
		blend_yuv(y_ptr, u_ptr, v_ptr, c->y, c->u, c->v, a);
		break;
	}
	case VIDEO_FORMAT_I422: {
		uint8_t *y_ptr = f->data[0] + y * f->linesize[0] + x;
		uint8_t *u_ptr = f->data[1] + y * f->linesize[1] + (x / 2);
		uint8_t *v_ptr = f->data[2] + y * f->linesize[2] + (x / 2);
		blend_yuv(y_ptr, u_ptr, v_ptr, c->y, c->u, c->v, a);
		break;
	}
	case VIDEO_FORMAT_BGRA:
	case VIDEO_FORMAT_BGRX: {
		uint8_t *pix = f->data[0] + y * f->linesize[0] + x * 4;
		blend_bgra(pix, c->r, c->g, c->b, a);
		break;
	}
	case VIDEO_FORMAT_RGBA: {
		uint8_t *pix = f->data[0] + y * f->linesize[0] + x * 4;
		uint32_t inv_a = 255 - a;
		pix[0] = (uint8_t)((pix[0] * inv_a + c->r * a) / 255);
		pix[1] = (uint8_t)((pix[1] * inv_a + c->g * a) / 255);
		pix[2] = (uint8_t)((pix[2] * inv_a + c->b * a) / 255);
		break;
	}
	case VIDEO_FORMAT_YUY2: {
		uint8_t *p = f->data[0] + y * f->linesize[0] + (x / 2) * 4;
		uint8_t *y_ptr = (x % 2 == 0) ? &p[0] : &p[2];
		uint8_t *u_ptr = &p[1];
		uint8_t *v_ptr = &p[3];
		blend_yuv(y_ptr, u_ptr, v_ptr, c->y, c->u, c->v, a);
		break;
	}
	default:
		break;
	}
}

static void draw_filled_circle(struct obs_source_frame *f, int cx, int cy, int radius,
                               const struct color_def *c, uint8_t a)
{
	int r2 = radius * radius;
	for (int dy = -radius; dy <= radius; dy++) {
		int max_dx = (int)sqrtf((float)(r2 - dy * dy));
		for (int dx = -max_dx; dx <= max_dx; dx++) {
			float dist = sqrtf((float)(dx * dx + dy * dy));
			float falloff = 1.0f;
			if (dist > (float)(radius - 1)) {
				falloff = (float)radius - dist;
				if (falloff < 0.0f) falloff = 0.0f;
			}
			uint8_t alpha = (uint8_t)((float)a * falloff);
			draw_point_pixel(f, cx + dx, cy + dy, c, alpha);
		}
	}
}

/* ------------------------------------------------------------------ */
/*  Bézier trajectory evaluation                                      */
/* ------------------------------------------------------------------ */
struct pt2d {
	float x, y;
};

struct traj_point {
	float x, y;
	float nx, ny;
};

static inline struct pt2d eval_bezier2(struct pt2d p0, struct pt2d c, struct pt2d p1, float t)
{
	float it = 1.0f - t;
	struct pt2d r;
	r.x = it * it * p0.x + 2.0f * it * t * c.x + t * t * p1.x;
	r.y = it * it * p0.y + 2.0f * it * t * c.y + t * t * p1.y;
	return r;
}

/* ------------------------------------------------------------------ */
/*  Frame overlay renderer                                            */
/* ------------------------------------------------------------------ */
struct obs_source_frame *ball_tracker_render(struct ball_tracker *bt, struct obs_source_frame *frame)
{
	if (!bt || !bt->enabled || bt->point_count < 3 || !frame)
		return frame;

	pthread_mutex_lock(&bt->mutex);

	struct pt2d p0 = {bt->points[0].u * (float)frame->width, bt->points[0].v * (float)frame->height};
	struct pt2d p1 = {bt->points[1].u * (float)frame->width, bt->points[1].v * (float)frame->height};
	struct pt2d p2 = {bt->points[2].u * (float)frame->width, bt->points[2].v * (float)frame->height};
	const struct tracker_color_set *cset = &s_color_sets[bt->color % BALL_TRACKER_COLOR_COUNT];

	float progress = 1.0f;
	if (bt->is_animating) {
		uint64_t now_ns = os_gettime_ns();
		if (now_ns >= bt->anim_start_ns + bt->anim_duration_ns) {
			bt->is_animating = false;
			progress = 1.0f;
		} else {
			progress = (float)(now_ns - bt->anim_start_ns) / (float)bt->anim_duration_ns;
			if (progress < 0.0f) progress = 0.0f;
			if (progress > 1.0f) progress = 1.0f;
		}
	}

	pthread_mutex_unlock(&bt->mutex);

	/* Always copy to bt->overlay_frame before drawing to guarantee
	 * that raw stored replay buffer frames and zoomed_frames are NEVER modified! */
	if (!bt->overlay_frame ||
	    bt->overlay_frame->width != frame->width ||
	    bt->overlay_frame->height != frame->height ||
	    bt->overlay_frame->format != frame->format) {
		if (bt->overlay_frame) {
			obs_source_frame_destroy(bt->overlay_frame);
			bt->overlay_frame = NULL;
		}
		bt->overlay_frame = obs_source_frame_create(frame->format, frame->width, frame->height);
	}
	struct obs_source_frame *target = frame;
	if (bt->overlay_frame) {
		obs_source_frame_copy(bt->overlay_frame, frame);
		target = bt->overlay_frame;
	}

	/* Scale thickness based on video height (sleek & smooth broadcast ribbon):
	 * 1080p -> ~6-7px radius (12-14px thick glossy capsule ribbon)
	 * 720p  -> ~4-5px radius (8-10px thick)
	 * 4K    -> ~13-14px radius (26-28px thick) */
	int radius_core = (int)((float)frame->height / 160.0f);
	if (radius_core < 3) radius_core = 3;
	if (radius_core > 14) radius_core = 14;

	/* Segment 1: Release -> Pitch (natural curved ballistic arc) */
	struct pt2d c1;
	c1.x = p0.x + (p1.x - p0.x) * 0.48f;
	float arc1 = fabsf(p1.x - p0.x) * 0.08f;
	if (arc1 < 25.0f) arc1 = 25.0f;
	c1.y = ((p0.y < p1.y) ? p0.y : p1.y) - arc1;

	float d1 = sqrtf((p1.x - p0.x) * (p1.x - p0.x) + (p1.y - p0.y) * (p1.y - p0.y));
	int steps1 = (int)(d1 / ((float)radius_core * 0.35f));
	if (steps1 < 90) steps1 = 90;
	if (steps1 > 350) steps1 = 350;

	struct traj_point pts1[380];
	for (int i = 0; i <= steps1; i++) {
		float t = (float)i / (float)steps1;
		struct pt2d pt = eval_bezier2(p0, c1, p1, t);
		pts1[i].x = pt.x;
		pts1[i].y = pt.y;

		/* Tangent derivative of quadratic Bézier */
		float tx = 2.0f * (1.0f - t) * (c1.x - p0.x) + 2.0f * t * (p1.x - c1.x);
		float ty = 2.0f * (1.0f - t) * (c1.y - p0.y) + 2.0f * t * (p1.y - c1.y);
		/* Perpendicular normal pointing upwards towards top of screen for light */
		float nx = -ty;
		float ny = tx;
		if (ny > 0.0f) { nx = -nx; ny = -ny; }
		float nlen = sqrtf(nx * nx + ny * ny);
		if (nlen > 0.001f) {
			pts1[i].nx = nx / nlen;
			pts1[i].ny = ny / nlen;
		} else {
			pts1[i].nx = 0.0f;
			pts1[i].ny = -1.0f;
		}
	}

	/* Segment 2: Pitch -> Stumps (DIRECT STRAIGHT LINE - no curve as requested) */
	float d2 = sqrtf((p2.x - p1.x) * (p2.x - p1.x) + (p2.y - p1.y) * (p2.y - p1.y));
	int steps2 = (int)(d2 / ((float)radius_core * 0.35f));
	if (steps2 < 70) steps2 = 70;
	if (steps2 > 280) steps2 = 280;

	float tx2 = p2.x - p1.x;
	float ty2 = p2.y - p1.y;
	float nx2 = -ty2;
	float ny2 = tx2;
	if (ny2 > 0.0f) { nx2 = -nx2; ny2 = -ny2; }
	float nlen2 = sqrtf(nx2 * nx2 + ny2 * ny2);
	if (nlen2 > 0.001f) {
		nx2 /= nlen2;
		ny2 /= nlen2;
	} else {
		nx2 = 0.0f;
		ny2 = -1.0f;
	}

	struct traj_point pts2[300];
	for (int i = 0; i <= steps2; i++) {
		float t = (float)i / (float)steps2;
		pts2[i].x = p1.x + (p2.x - p1.x) * t;
		pts2[i].y = p1.y + (p2.y - p1.y) * t;
		pts2[i].nx = nx2;
		pts2[i].ny = ny2;
	}

	/* Ballistic trajectory proportion between Segment 1 (Release -> Pitch) and Segment 2 (Pitch -> Stumps) */
	float total_d = d1 + d2;
	float t_split = 0.60f;
	if (total_d > 1.0f) {
		t_split = d1 / total_d;
		if (t_split < 0.35f) t_split = 0.35f;
		if (t_split > 0.80f) t_split = 0.80f;
	}

	int active_steps1 = 0;
	int active_steps2 = -1;
	struct pt2d head_pos = p0;

	if (progress <= t_split) {
		float u1 = progress / t_split;
		active_steps1 = (int)(u1 * (float)steps1);
		if (active_steps1 < 0) active_steps1 = 0;
		if (active_steps1 > steps1) active_steps1 = steps1;
		head_pos.x = pts1[active_steps1].x;
		head_pos.y = pts1[active_steps1].y;
		active_steps2 = -1;
	} else {
		active_steps1 = steps1;
		float u2 = (progress - t_split) / (1.0f - t_split);
		if (u2 < 0.0f) u2 = 0.0f;
		if (u2 > 1.0f) u2 = 1.0f;
		active_steps2 = (int)(u2 * (float)steps2);
		if (active_steps2 < 0) active_steps2 = 0;
		if (active_steps2 > steps2) active_steps2 = steps2;
		head_pos.x = pts2[active_steps2].x;
		head_pos.y = pts2[active_steps2].y;
	}

	/* --- 4-Pass 3D Glossy Capsule Ribbon Shading --- */

	/* Pass 1: Soft Ambient Outer Glow Aura */
	int radius_glow = radius_core + 3;
	for (int i = 0; i <= active_steps1; i++)
		draw_filled_circle(target, (int)pts1[i].x, (int)pts1[i].y, radius_glow, &cset->base, 55);
	if (active_steps2 >= 0) {
		for (int i = 0; i <= active_steps2; i++)
			draw_filled_circle(target, (int)pts2[i].x, (int)pts2[i].y, radius_glow, &cset->base, 55);
	}

	/* Pass 2: Solid Vibrant Capsule Core Body (Smooth rounded cylinder/capsule ends) */
	for (int i = 0; i <= active_steps1; i++)
		draw_filled_circle(target, (int)pts1[i].x, (int)pts1[i].y, radius_core, &cset->base, 255);
	if (active_steps2 >= 0) {
		for (int i = 0; i <= active_steps2; i++)
			draw_filled_circle(target, (int)pts2[i].x, (int)pts2[i].y, radius_core, &cset->base, 255);
	}

	/* Pass 3: Soft Glossy Cylindrical Sheen (Pastel gradient highlight on upper ridge) */
	int r_sheen = (int)((float)radius_core * 0.55f);
	if (r_sheen < 1) r_sheen = 1;
	float off_sheen = (float)radius_core * 0.28f;
	for (int i = 0; i <= active_steps1; i++) {
		int sx = (int)(pts1[i].x + pts1[i].nx * off_sheen);
		int sy = (int)(pts1[i].y + pts1[i].ny * off_sheen);
		draw_filled_circle(target, sx, sy, r_sheen, &cset->highlight, 160);
	}
	if (active_steps2 >= 0) {
		for (int i = 0; i <= active_steps2; i++) {
			int sx = (int)(pts2[i].x + pts2[i].nx * off_sheen);
			int sy = (int)(pts2[i].y + pts2[i].ny * off_sheen);
			draw_filled_circle(target, sx, sy, r_sheen, &cset->highlight, 160);
		}
	}

	/* Pass 4: Sharp Specular White Reflection Streak (Glossy shine like reference photo) */
	int r_spec = (int)((float)radius_core * 0.28f);
	if (r_spec < 1) r_spec = 1;
	float off_spec = (float)radius_core * 0.40f;
	for (int i = 0; i <= active_steps1; i++) {
		int hx = (int)(pts1[i].x + pts1[i].nx * off_spec);
		int hy = (int)(pts1[i].y + pts1[i].ny * off_spec);
		draw_filled_circle(target, hx, hy, r_spec, &s_white, 220);
	}
	if (active_steps2 >= 0) {
		for (int i = 0; i <= active_steps2; i++) {
			int hx = (int)(pts2[i].x + pts2[i].nx * off_spec);
			int hy = (int)(pts2[i].y + pts2[i].ny * off_spec);
			draw_filled_circle(target, hx, hy, r_spec, &s_white, 220);
		}
	}

	/* Point 1: 3D Glossy Release Ball Sphere (Bowler's release point) */
	int ball_r = (int)((float)radius_core * 1.6f);
	if (ball_r < 5) ball_r = 5;
	draw_filled_circle(target, (int)p0.x, (int)p0.y, ball_r + 4, &cset->base, 100);
	draw_filled_circle(target, (int)p0.x, (int)p0.y, ball_r, &cset->base, 255);
	draw_filled_circle(target, (int)p0.x - ball_r / 3, (int)p0.y - ball_r / 3, ball_r * 2 / 3, &cset->highlight, 170);
	draw_filled_circle(target, (int)p0.x - ball_r / 3, (int)p0.y - ball_r / 3, ball_r / 3, &s_white, 230);

	/* NO DOT AT PITCH POINT (p1) - as requested by user */

	/* Moving Ball during animation / Stumps Impact Ball when complete */
	if (progress < 0.98f) {
		if (progress > 0.02f) {
			draw_filled_circle(target, (int)head_pos.x, (int)head_pos.y, ball_r + 4, &cset->base, 100);
			draw_filled_circle(target, (int)head_pos.x, (int)head_pos.y, ball_r, &cset->base, 255);
			draw_filled_circle(target, (int)head_pos.x - ball_r / 3, (int)head_pos.y - ball_r / 3, ball_r * 2 / 3, &cset->highlight, 170);
			draw_filled_circle(target, (int)head_pos.x - ball_r / 3, (int)head_pos.y - ball_r / 3, ball_r / 3, &s_white, 230);
		}
	} else {
		/* Point 3: 3D Glossy Stumps Impact / Target Ball */
		draw_filled_circle(target, (int)p2.x, (int)p2.y, ball_r + 4, &cset->base, 100);
		draw_filled_circle(target, (int)p2.x, (int)p2.y, ball_r, &cset->base, 255);
		draw_filled_circle(target, (int)p2.x - ball_r / 3, (int)p2.y - ball_r / 3, ball_r * 2 / 3, &cset->highlight, 170);
		draw_filled_circle(target, (int)p2.x - ball_r / 3, (int)p2.y - ball_r / 3, ball_r / 3, &s_white, 230);
	}

	return target;
}

/* ------------------------------------------------------------------ */
/*  Win32 UI Implementation                                           */
/* ------------------------------------------------------------------ */
static inline void ball_tracker_start_animation_locked(struct ball_tracker *bt)
{
	if (bt->point_count == 3) {
		bt->is_animating = true;
		bt->anim_start_ns = os_gettime_ns();
		bt->anim_duration_ns = 1200ULL * 1000000ULL;
	}
}

static void update_status_text(struct ball_tracker *bt)
{
	if (!bt || !bt->hwnd_status)
		return;

	char buf[128];
	switch (bt->point_count) {
	case 0:
		snprintf(buf, sizeof(buf), ">> Step 1/3: Click ball at RELEASE (Bowler's hand)");
		break;
	case 1:
		snprintf(buf, sizeof(buf), ">> Step 2/3: Click ball where it PITCHED / BOUNCED on turf");
		break;
	case 2:
		snprintf(buf, sizeof(buf), ">> Step 3/3: Click ball at STUMPS / IMPACT point");
		break;
	default:
		if (bt->enabled)
			snprintf(buf, sizeof(buf), "[OK] Ball Tracking Active! [Visible on Replay]");
		else
			snprintf(buf, sizeof(buf), "[Hidden] Ball Tracking Hidden (Click 'Show Line' to show)");
		break;
	}
	SetWindowTextA(bt->hwnd_status, buf);

	if (bt->hwnd_btn_toggle) {
		SetWindowTextW(bt->hwnd_btn_toggle, bt->enabled ? L"Line: ON" : L"Line: OFF");
	}
	if (bt->hwnd_btn_color) {
		const struct tracker_color_set *cset = &s_color_sets[bt->color % BALL_TRACKER_COLOR_COUNT];
		SetWindowTextW(bt->hwnd_btn_color, cset->name);
	}
}

static void trigger_paused_rerender(struct ball_tracker *bt)
{
	if (!bt || !bt->context)
		return;
	struct replay_source *c = bt->context;
	if (!c->play && c->current_replay.video_frame_count) {
		if (pthread_mutex_trylock(&c->video_mutex) != 0) {
			c->stepped = true;
			return;
		}
		uint64_t pos = c->video_frame_position;
		if (pos >= c->current_replay.video_frame_count)
			pos = c->current_replay.video_frame_count - 1;
		struct obs_source_frame *f = c->current_replay.video_frames[pos];
		if (f) {
			uint64_t os_ts = obs_get_video_frame_time();
			uint64_t orig_ts = f->timestamp;
			f->timestamp = os_ts;
			struct obs_source_frame *out = replay_get_zoomed_frame(c, f);
			out = ball_tracker_render(bt, out);
			obs_source_output_video(c->source, out);
			f->timestamp = orig_ts;
			c->stepped = true;
		}
		pthread_mutex_unlock(&c->video_mutex);
	}
}

static void ball_tracker_layout(struct ball_tracker *bt, int client_w, int client_h)
{
	if (!bt || !bt->hwnd || client_w <= 0 || client_h <= 0)
		return;

	const int margin_x = 14;
	const int status_y = 10;
	const int status_h = 26;

	if (bt->hwnd_status) {
		SetWindowPos(bt->hwnd_status, NULL,
			margin_x, status_y,
			client_w - 2 * margin_x, status_h,
			SWP_NOZORDER | SWP_NOACTIVATE);
	}

	const int btn_h = 30;
	const int row_gap = 8;
	const int bottom_margin = 12;

	int btn_y2 = client_h - bottom_margin - btn_h;
	int btn_y1 = btn_y2 - row_gap - btn_h;

	/* Row 1 buttons (5 buttons: Prev5, Prev1, Play/Pause, Next1, Next5) */
	HWND row1_btns[] = {
		bt->hwnd_btn_prev5, bt->hwnd_btn_prev1,
		bt->hwnd_btn_play_pause,
		bt->hwnd_btn_next1, bt->hwnd_btn_next5
	};
	int total_avail_w = client_w - 2 * margin_x;
	int gap1 = 8;
	int btn_w1 = (total_avail_w - 4 * gap1) / 5;
	if (btn_w1 < 50) btn_w1 = 50;
	for (int i = 0; i < 5; i++) {
		if (row1_btns[i]) {
			int bx = margin_x + i * (btn_w1 + gap1);
			SetWindowPos(row1_btns[i], NULL, bx, btn_y1, btn_w1, btn_h,
				SWP_NOZORDER | SWP_NOACTIVATE);
		}
	}

	/* Row 2 buttons (5 buttons: Reset, Animate, Line Toggle, Color, Close) */
	HWND row2_btns[] = {
		bt->hwnd_btn_reset, bt->hwnd_btn_animate,
		bt->hwnd_btn_toggle, bt->hwnd_btn_color,
		bt->hwnd_btn_close
	};
	int gap2 = 8;
	int btn_w2 = (total_avail_w - 4 * gap2) / 5;
	if (btn_w2 < 50) btn_w2 = 50;
	for (int i = 0; i < 5; i++) {
		if (row2_btns[i]) {
			int bx = margin_x + i * (btn_w2 + gap2);
			SetWindowPos(row2_btns[i], NULL, bx, btn_y2, btn_w2, btn_h,
				SWP_NOZORDER | SWP_NOACTIVATE);
		}
	}

	/* Preview canvas area */
	int top_limit = status_y + status_h + 8;
	int bot_limit = btn_y1 - 10;
	int avail_h = bot_limit - top_limit;
	int avail_w = client_w - 2 * margin_x;

	if (avail_w < 50) avail_w = 50;
	if (avail_h < 50) avail_h = 50;

	/* Fit 16:9 aspect ratio inside avail_w x avail_h */
	int target_w = avail_w;
	int target_h = (int)((float)target_w * 9.0f / 16.0f);
	if (target_h > avail_h) {
		target_h = avail_h;
		target_w = (int)((float)target_h * 16.0f / 9.0f);
	}
	if (target_w < 16) target_w = 16;
	if (target_h < 9)  target_h = 9;

	bt->preview_x = margin_x + (avail_w - target_w) / 2;
	bt->preview_y = top_limit + (avail_h - target_h) / 2;
	bt->preview_w = target_w;
	bt->preview_h = target_h;
}

static void update_preview_buffer(struct ball_tracker *bt)
{
	if (!bt || !bt->context)
		return;
	struct replay_source *c = bt->context;
	if (!c->current_replay.video_frame_count)
		return;

	uint64_t pos = c->video_frame_position;
	if (pos >= c->current_replay.video_frame_count)
		pos = c->current_replay.video_frame_count - 1;
	struct obs_source_frame *src = c->current_replay.video_frames[pos];
	if (!src) return;

	int pw = bt->preview_w > 0 ? bt->preview_w : 760;
	int ph = bt->preview_h > 0 ? bt->preview_h : 428;

	uint32_t target_linesize = (uint32_t)pw * 4;
	if (!bt->preview_pixels || bt->preview_linesize != target_linesize ||
	    bt->preview_scaler_target_h != (uint32_t)ph) {
		if (bt->preview_pixels)
			bfree(bt->preview_pixels);
		bt->preview_linesize = target_linesize;
		bt->preview_pixels = bzalloc(bt->preview_linesize * ph);
	}

	if (!bt->preview_scaler ||
	    bt->preview_scaler_w != src->width ||
	    bt->preview_scaler_h != src->height ||
	    bt->preview_scaler_format != src->format ||
	    bt->preview_scaler_target_w != (uint32_t)pw ||
	    bt->preview_scaler_target_h != (uint32_t)ph) {
		if (bt->preview_scaler) {
			video_scaler_destroy(bt->preview_scaler);
			bt->preview_scaler = NULL;
		}

		struct obs_video_info ovi;
		enum video_range_type range = VIDEO_RANGE_DEFAULT;
		enum video_colorspace cs = VIDEO_CS_DEFAULT;
		if (obs_get_video_info(&ovi)) {
			range = ovi.range;
			cs = ovi.colorspace;
		}

		struct video_scale_info ssi;
		memset(&ssi, 0, sizeof(ssi));
		ssi.format = src->format;
		ssi.width = src->width;
		ssi.height = src->height;
		ssi.range = range;
		ssi.colorspace = cs;

		struct video_scale_info dsi;
		memset(&dsi, 0, sizeof(dsi));
		dsi.format = VIDEO_FORMAT_BGRA;
		dsi.width = pw;
		dsi.height = ph;
		dsi.range = range;
		dsi.colorspace = cs;

		int ret = video_scaler_create(&bt->preview_scaler, &dsi, &ssi, VIDEO_SCALE_FAST_BILINEAR);
		if (ret != VIDEO_SCALER_SUCCESS)
			return;

		bt->preview_scaler_w = src->width;
		bt->preview_scaler_h = src->height;
		bt->preview_scaler_format = src->format;
		bt->preview_scaler_target_w = (uint32_t)pw;
		bt->preview_scaler_target_h = (uint32_t)ph;
	}

	uint8_t *dst_data[1] = {bt->preview_pixels};
	uint32_t dst_linesize[1] = {bt->preview_linesize};
	video_scaler_scale(bt->preview_scaler, dst_data, dst_linesize,
	                   (const uint8_t *const *)src->data, src->linesize);
}

static LRESULT CALLBACK BallTrackerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	struct ball_tracker *bt = (struct ball_tracker *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);

	switch (msg) {
	case WM_CREATE: {
		CREATESTRUCTA *cs = (CREATESTRUCTA *)lParam;
		bt = (struct ball_tracker *)cs->lpCreateParams;
		SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)bt);
		bt->hwnd = hwnd;

		bt->hbr_bg = CreateSolidBrush(RGB(20, 22, 28));
		bt->hbr_canvas_border = CreateSolidBrush(RGB(50, 55, 65));

		bt->hfont_status = CreateFontA(16, 0, 0, 0, FW_BOLD, 0, 0, 0,
			DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
			DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
		bt->hfont_btns = CreateFontA(13, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
			DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
			DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");

		/* Top status text */
		bt->hwnd_status = CreateWindowExA(0, "STATIC", "",
			WS_CHILD | WS_VISIBLE | SS_CENTER,
			0, 0, 0, 0, hwnd, NULL, NULL, NULL);
		SendMessageA(bt->hwnd_status, WM_SETFONT, (WPARAM)bt->hfont_status, TRUE);

		/* Control buttons Row 1: Scrubbing */
		bt->hwnd_btn_prev5 = CreateWindowExW(0, L"BUTTON", L"\u25c4 -5",
			WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
			0, 0, 0, 0, hwnd, (HMENU)ID_BTN_PREV5, NULL, NULL);
		bt->hwnd_btn_prev1 = CreateWindowExW(0, L"BUTTON", L"\u25c4 -1",
			WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
			0, 0, 0, 0, hwnd, (HMENU)ID_BTN_PREV1, NULL, NULL);
		bt->hwnd_btn_play_pause = CreateWindowExW(0, L"BUTTON", L"|| Pause",
			WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
			0, 0, 0, 0, hwnd, (HMENU)ID_BTN_PLAY_PAUSE, NULL, NULL);
		bt->hwnd_btn_next1 = CreateWindowExW(0, L"BUTTON", L"+1 \u25ba",
			WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
			0, 0, 0, 0, hwnd, (HMENU)ID_BTN_NEXT1, NULL, NULL);
		bt->hwnd_btn_next5 = CreateWindowExW(0, L"BUTTON", L"+5 \u25ba",
			WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
			0, 0, 0, 0, hwnd, (HMENU)ID_BTN_NEXT5, NULL, NULL);

		/* Control buttons Row 2: Tracker controls */
		bt->hwnd_btn_reset = CreateWindowExW(0, L"BUTTON", L"\u21bb Reset",
			WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
			0, 0, 0, 0, hwnd, (HMENU)ID_BTN_RESET, NULL, NULL);
		bt->hwnd_btn_animate = CreateWindowExW(0, L"BUTTON", L"\u25b6 Animate",
			WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
			0, 0, 0, 0, hwnd, (HMENU)ID_BTN_ANIMATE, NULL, NULL);
		bt->hwnd_btn_toggle = CreateWindowExW(0, L"BUTTON", L"Line: ON",
			WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
			0, 0, 0, 0, hwnd, (HMENU)ID_BTN_TOGGLE, NULL, NULL);
		bt->hwnd_btn_color = CreateWindowExW(0, L"BUTTON", L"Color: Red",
			WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
			0, 0, 0, 0, hwnd, (HMENU)ID_BTN_COLOR, NULL, NULL);
		bt->hwnd_btn_close = CreateWindowExW(0, L"BUTTON", L"\u2715 Close",
			WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
			0, 0, 0, 0, hwnd, (HMENU)ID_BTN_CLOSE, NULL, NULL);

		HWND all_btns[] = {bt->hwnd_btn_prev5, bt->hwnd_btn_prev1, bt->hwnd_btn_play_pause,
		                   bt->hwnd_btn_next1, bt->hwnd_btn_next5, bt->hwnd_btn_reset,
		                   bt->hwnd_btn_animate, bt->hwnd_btn_toggle, bt->hwnd_btn_color, bt->hwnd_btn_close};
		for (int i = 0; i < 10; i++) {
			SendMessageA(all_btns[i], WM_SETFONT, (WPARAM)bt->hfont_btns, TRUE);
		}

		RECT rcClient;
		GetClientRect(hwnd, &rcClient);
		ball_tracker_layout(bt, rcClient.right - rcClient.left, rcClient.bottom - rcClient.top);

		update_status_text(bt);
		return 0;
	}

	case WM_GETMINMAXINFO: {
		MINMAXINFO *mmi = (MINMAXINFO *)lParam;
		mmi->ptMinTrackSize.x = 640;
		mmi->ptMinTrackSize.y = 480;
		return 0;
	}

	case WM_SIZE: {
		if (bt) {
			int w = LOWORD(lParam);
			int h = HIWORD(lParam);
			ball_tracker_layout(bt, w, h);
			InvalidateRect(hwnd, NULL, FALSE);
		}
		return 0;
	}

	case WM_WINDOWPOSCHANGED: {
		WINDOWPOS *wp = (WINDOWPOS *)lParam;
		if (bt && (!(wp->flags & SWP_NOMOVE) || !(wp->flags & SWP_NOSIZE))) {
			RECT wr;
			GetWindowRect(hwnd, &wr);
			int ww = wr.right - wr.left;
			int wh = wr.bottom - wr.top;
			if (ww >= 500 && wh >= 400 && !IsIconic(hwnd)) {
				bt->saved_x = wr.left;
				bt->saved_y = wr.top;
				bt->saved_w = ww;
				bt->saved_h = wh;
				bt->has_saved_pos = true;

				s_bt_saved_x = wr.left;
				s_bt_saved_y = wr.top;
				s_bt_saved_w = ww;
				s_bt_saved_h = wh;
				s_bt_has_saved_pos = true;

				if (bt->context && bt->context->source) {
					obs_data_t *settings = obs_source_get_settings(bt->context->source);
					if (settings) {
						obs_data_set_int(settings, "ball_tracker_win_x", bt->saved_x);
						obs_data_set_int(settings, "ball_tracker_win_y", bt->saved_y);
						obs_data_set_int(settings, "ball_tracker_win_w", bt->saved_w);
						obs_data_set_int(settings, "ball_tracker_win_h", bt->saved_h);
						obs_data_release(settings);
					}
				}
			}
		}
		break;
	}

	case WM_CTLCOLORSTATIC: {
		HDC hdc = (HDC)wParam;
		if (bt && (HWND)lParam == bt->hwnd_status) {
			if (bt->point_count == 3 && bt->enabled)
				SetTextColor(hdc, RGB(50, 230, 80));
			else if (bt->point_count == 0)
				SetTextColor(hdc, RGB(0, 200, 255));
			else if (bt->point_count == 1)
				SetTextColor(hdc, RGB(255, 210, 40));
			else
				SetTextColor(hdc, RGB(255, 100, 100));
			SetBkColor(hdc, RGB(20, 22, 28));
			return (LRESULT)bt->hbr_bg;
		}
		break;
	}

	case WM_ERASEBKGND:
		return 1;

	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC hdc = BeginPaint(hwnd, &ps);
		if (!bt) {
			EndPaint(hwnd, &ps);
			return 0;
		}

		RECT rc;
		GetClientRect(hwnd, &rc);
		FillRect(hdc, &rc, bt->hbr_bg);

		/* Render preview frame */
		update_preview_buffer(bt);
		int cx = bt->preview_x;
		int cy = bt->preview_y;
		int pw = bt->preview_w;
		int ph = bt->preview_h;

		if (bt->preview_pixels && pw > 0 && ph > 0) {
			BITMAPINFO bmi = {0};
			bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
			bmi.bmiHeader.biWidth = pw;
			bmi.bmiHeader.biHeight = -ph;
			bmi.bmiHeader.biPlanes = 1;
			bmi.bmiHeader.biBitCount = 32;
			bmi.bmiHeader.biCompression = BI_RGB;

			StretchDIBits(hdc, cx, cy, pw, ph,
			              0, 0, pw, ph,
			              bt->preview_pixels, &bmi, DIB_RGB_COLORS, SRCCOPY);
		} else if (pw > 0 && ph > 0) {
			RECT rcBox = {cx, cy, cx + pw, cy + ph};
			FillRect(hdc, &rcBox, bt->hbr_canvas_border);
		}

		/* Draw trajectory line & markers on canvas DC */
		if (bt->point_count >= 1 && pw > 0 && ph > 0) {
			const struct tracker_color_set *cset = &s_color_sets[bt->color % BALL_TRACKER_COLOR_COUNT];
			int px0 = cx + (int)(bt->points[0].u * (float)pw);
			int py0 = cy + (int)(bt->points[0].v * (float)ph);

			if (bt->point_count >= 2) {
				int px1 = cx + (int)(bt->points[1].u * (float)pw);
				int py1 = cy + (int)(bt->points[1].v * (float)ph);

				/* Parabolic Bézier arc 1: Release -> Pitch */
				struct pt2d c1;
				c1.x = (float)px0 + (float)(px1 - px0) * 0.48f;
				float a1 = fabsf((float)(px1 - px0)) * 0.08f;
				if (a1 < 10.0f) a1 = 10.0f;
				c1.y = ((py0 < py1) ? (float)py0 : (float)py1) - a1;

				struct pt2d start = {(float)px0, (float)py0};
				struct pt2d end1 = {(float)px1, (float)py1};

				/* 1. Base capsule line (sleek and smooth: 5px) */
				HPEN hPenBase = CreatePen(PS_SOLID, 5, cset->gdi_base);
				HPEN hOldPen = (HPEN)SelectObject(hdc, hPenBase);

				MoveToEx(hdc, px0, py0, NULL);
				for (int i = 1; i <= 60; i++) {
					struct pt2d p = eval_bezier2(start, c1, end1, (float)i / 60.0f);
					LineTo(hdc, (int)p.x, (int)p.y);
				}

				if (bt->point_count >= 3) {
					int px2 = cx + (int)(bt->points[2].u * (float)pw);
					int py2 = cy + (int)(bt->points[2].v * (float)ph);
					/* Segment 2: Pitch -> Stumps is a STRAIGHT LINE (no curve)! */
					LineTo(hdc, px2, py2);
				}

				/* 2. Glossy highlight streak on top (2px) */
				HPEN hPenGloss = CreatePen(PS_SOLID, 2, cset->gdi_highlight);
				SelectObject(hdc, hPenGloss);
				DeleteObject(hPenBase);

				MoveToEx(hdc, px0, py0 - 1, NULL);
				for (int i = 1; i <= 60; i++) {
					struct pt2d p = eval_bezier2(start, c1, end1, (float)i / 60.0f);
					LineTo(hdc, (int)p.x, (int)p.y - 1);
				}

				if (bt->point_count >= 3) {
					int px2 = cx + (int)(bt->points[2].u * (float)pw);
					int py2 = cy + (int)(bt->points[2].v * (float)ph);
					LineTo(hdc, px2, py2 - 1);
				}

				SelectObject(hdc, hOldPen);
				DeleteObject(hPenGloss);
			}

			/* Draw point badges: ① Release, ② Pitch, ③ Stumps */
			const char *labels[] = {"1. Release", "2. Pitch", "3. Stumps"};
			COLORREF bcols[] = {RGB(0, 220, 255), RGB(255, 210, 30), RGB(255, 50, 60)};
			SetBkMode(hdc, TRANSPARENT);
			SetTextColor(hdc, RGB(255, 255, 255));

			for (int i = 0; i < bt->point_count; i++) {
				int bx = cx + (int)(bt->points[i].u * (float)pw);
				int by = cy + (int)(bt->points[i].v * (float)ph);

				HBRUSH hbr = CreateSolidBrush(bcols[i]);
				HBRUSH hOldBr = (HBRUSH)SelectObject(hdc, hbr);
				HPEN hpw = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
				HPEN hOldP = (HPEN)SelectObject(hdc, hpw);

				Ellipse(hdc, bx - 6, by - 6, bx + 6, by + 6);
				TextOutA(hdc, bx + 8, by - 8, labels[i], (int)strlen(labels[i]));

				SelectObject(hdc, hOldP);
				DeleteObject(hpw);
				SelectObject(hdc, hOldBr);
				DeleteObject(hbr);
			}
		}

		EndPaint(hwnd, &ps);
		return 0;
	}

	case WM_LBUTTONDOWN: {
		if (!bt) return 0;
		int mx = LOWORD(lParam);
		int my = HIWORD(lParam);
		int cx = bt->preview_x;
		int cy = bt->preview_y;
		int pw = bt->preview_w;
		int ph = bt->preview_h;

		if (pw > 0 && ph > 0 && mx >= cx && mx <= cx + pw && my >= cy && my <= cy + ph) {
			float u = (float)(mx - cx) / (float)pw;
			float v = (float)(my - cy) / (float)ph;
			if (u < 0.0f) u = 0.0f;
			if (u > 1.0f) u = 1.0f;
			if (v < 0.0f) v = 0.0f;
			if (v > 1.0f) v = 1.0f;

			pthread_mutex_lock(&bt->mutex);
			if (bt->point_count < 3) {
				bt->points[bt->point_count].u = u;
				bt->points[bt->point_count].v = v;
				bt->points[bt->point_count].set = true;
				bt->point_count++;
				if (bt->point_count == 3) {
					bt->enabled = true;
					ball_tracker_start_animation_locked(bt);
				}
			} else {
				/* Cycle or fine tune: replace closest point */
				int closest = 0;
				float min_dist = 9999.0f;
				for (int i = 0; i < 3; i++) {
					float du = bt->points[i].u - u;
					float dv = bt->points[i].v - v;
					float d = du * du + dv * dv;
					if (d < min_dist) {
						min_dist = d;
						closest = i;
					}
				}
				bt->points[closest].u = u;
				bt->points[closest].v = v;
				ball_tracker_start_animation_locked(bt);
			}
			pthread_mutex_unlock(&bt->mutex);

			update_status_text(bt);
			InvalidateRect(hwnd, NULL, FALSE);
			trigger_paused_rerender(bt);
		}
		return 0;
	}

	case WM_COMMAND: {
		if (!bt) return 0;
		int id = LOWORD(wParam);
		switch (id) {
		case ID_BTN_PREV5:
			replay_step_frames(bt->context, true, false, 5);
			InvalidateRect(hwnd, NULL, FALSE);
			break;
		case ID_BTN_PREV1:
			replay_step_frames(bt->context, true, false, 1);
			InvalidateRect(hwnd, NULL, FALSE);
			break;
		case ID_BTN_PLAY_PAUSE:
			replay_toggle_play_pause(bt->context);
			InvalidateRect(hwnd, NULL, FALSE);
			break;
		case ID_BTN_NEXT1:
			replay_step_frames(bt->context, true, true, 1);
			InvalidateRect(hwnd, NULL, FALSE);
			break;
		case ID_BTN_NEXT5:
			replay_step_frames(bt->context, true, true, 5);
			InvalidateRect(hwnd, NULL, FALSE);
			break;
		case ID_BTN_RESET:
			ball_tracker_reset_points(bt);
			trigger_paused_rerender(bt);
			break;
		case ID_BTN_ANIMATE:
			if (bt->point_count == 3) {
				bt->enabled = true;
				ball_tracker_start_animation(bt);
				update_status_text(bt);
				InvalidateRect(hwnd, NULL, FALSE);
				trigger_paused_rerender(bt);
			}
			break;
		case ID_BTN_TOGGLE:
			ball_tracker_toggle_visibility(bt);
			InvalidateRect(hwnd, NULL, FALSE);
			trigger_paused_rerender(bt);
			break;
		case ID_BTN_COLOR:
			ball_tracker_cycle_color(bt);
			InvalidateRect(hwnd, NULL, FALSE);
			trigger_paused_rerender(bt);
			break;
		case ID_BTN_CLOSE:
			ShowWindow(hwnd, SW_HIDE);
			bt->is_showing = false;
			break;
		}
		return 0;
	}

	case WM_CLOSE:
		if (bt) {
			ShowWindow(hwnd, SW_HIDE);
			bt->is_showing = false;
		}
		return 0;

	case WM_DESTROY:
		if (bt) {
			if (bt->hfont_status) DeleteObject(bt->hfont_status);
			if (bt->hfont_btns)   DeleteObject(bt->hfont_btns);
			if (bt->hbr_bg)       DeleteObject(bt->hbr_bg);
			if (bt->hbr_canvas_border) DeleteObject(bt->hbr_canvas_border);
		}
		PostQuitMessage(0);
		return 0;
	}

	return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static DWORD WINAPI BallTrackerThread(LPVOID param)
{
	struct ball_tracker *bt = (struct ball_tracker *)param;
	WNDCLASSA wc = {0};
	MSG msg;

	wc.style         = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc   = BallTrackerWndProc;
	wc.hInstance     = GetModuleHandle(NULL);
	wc.lpszClassName = "OBSReplayBallTrackerPopup";
	wc.hCursor       = LoadCursor(NULL, IDC_CROSS);
	wc.hbrBackground = NULL;
	RegisterClassA(&wc);

	int init_w = s_bt_saved_w >= 640 ? s_bt_saved_w : 840;
	int init_h = s_bt_saved_h >= 480 ? s_bt_saved_h : 620;
	int init_x = s_bt_has_saved_pos ? s_bt_saved_x : CW_USEDEFAULT;
	int init_y = s_bt_has_saved_pos ? s_bt_saved_y : CW_USEDEFAULT;

	HWND hwnd = CreateWindowExA(
		WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
		"OBSReplayBallTrackerPopup",
		"Cricket Ball Tracker (3-Point)",
		WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX,
		init_x, init_y, init_w, init_h,
		NULL, NULL, GetModuleHandle(NULL), bt);

	if (!hwnd) {
		UnregisterClassA("OBSReplayBallTrackerPopup", GetModuleHandle(NULL));
		return 0;
	}

	while (GetMessageA(&msg, NULL, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessageA(&msg);
	}

	UnregisterClassA("OBSReplayBallTrackerPopup", GetModuleHandle(NULL));
	return 0;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */
struct ball_tracker *ball_tracker_create(struct replay_source *context)
{
	struct ball_tracker *bt = bzalloc(sizeof(*bt));
	bt->context = context;
	pthread_mutex_init(&bt->mutex, NULL);
	bt->color = BALL_TRACKER_COLOR_MAGENTA;
	bt->thickness = 5;
	bt->enabled = false;
	bt->point_count = 0;

	if (context && context->source) {
		obs_data_t *settings = obs_source_get_settings(context->source);
		if (settings) {
			int sx = (int)obs_data_get_int(settings, "ball_tracker_win_x");
			int sy = (int)obs_data_get_int(settings, "ball_tracker_win_y");
			int sw = (int)obs_data_get_int(settings, "ball_tracker_win_w");
			int sh = (int)obs_data_get_int(settings, "ball_tracker_win_h");
			if (sw >= 640 && sh >= 480) {
				bt->saved_x = sx;
				bt->saved_y = sy;
				bt->saved_w = sw;
				bt->saved_h = sh;
				bt->has_saved_pos = true;

				s_bt_saved_x = sx;
				s_bt_saved_y = sy;
				s_bt_saved_w = sw;
				s_bt_saved_h = sh;
				s_bt_has_saved_pos = true;
			}
			obs_data_release(settings);
		}
	}

	bt->thread = CreateThread(NULL, 0, BallTrackerThread, bt, 0, &bt->thread_id);
	return bt;
}

void ball_tracker_destroy(struct ball_tracker *bt)
{
	if (!bt) return;
	if (bt->hwnd)
		PostMessageA(bt->hwnd, WM_DESTROY, 0, 0);
	if (bt->thread) {
		WaitForSingleObject(bt->thread, 2000);
		CloseHandle(bt->thread);
	}
	pthread_mutex_destroy(&bt->mutex);
	if (bt->overlay_frame) {
		obs_source_frame_destroy(bt->overlay_frame);
		bt->overlay_frame = NULL;
	}
	if (bt->preview_scaler) {
		video_scaler_destroy(bt->preview_scaler);
		bt->preview_scaler = NULL;
	}
	if (bt->preview_pixels) {
		bfree(bt->preview_pixels);
		bt->preview_pixels = NULL;
	}
	bfree(bt);
}

void ball_tracker_show(struct ball_tracker *bt)
{
	if (!bt) return;
	for (int i = 0; !bt->hwnd && i < 20; i++)
		os_sleep_ms(10);
	if (bt->hwnd) {
		if (bt->has_saved_pos || s_bt_has_saved_pos) {
			int sx = bt->has_saved_pos ? bt->saved_x : s_bt_saved_x;
			int sy = bt->has_saved_pos ? bt->saved_y : s_bt_saved_y;
			int sw = bt->has_saved_pos ? bt->saved_w : s_bt_saved_w;
			int sh = bt->has_saved_pos ? bt->saved_h : s_bt_saved_h;
			if (sw < 640) sw = 840;
			if (sh < 480) sh = 620;
			SetWindowPos(bt->hwnd, HWND_TOPMOST, sx, sy, sw, sh,
			             SWP_SHOWWINDOW);
		} else {
			SetWindowPos(bt->hwnd, HWND_TOPMOST, 0, 0, 0, 0,
			             SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
		}
		bt->is_showing = true;
		update_status_text(bt);
		InvalidateRect(bt->hwnd, NULL, FALSE);
	}
}

void ball_tracker_hide(struct ball_tracker *bt)
{
	if (!bt || !bt->hwnd) return;
	ShowWindow(bt->hwnd, SW_HIDE);
	bt->is_showing = false;
}

void ball_tracker_reset_points(struct ball_tracker *bt)
{
	if (!bt) return;
	pthread_mutex_lock(&bt->mutex);
	for (int i = 0; i < 3; i++) {
		bt->points[i].set = false;
		bt->points[i].u = 0.0f;
		bt->points[i].v = 0.0f;
	}
	bt->point_count = 0;
	bt->enabled = false;
	bt->is_animating = false;
	pthread_mutex_unlock(&bt->mutex);

	update_status_text(bt);
	if (bt->hwnd && bt->is_showing)
		InvalidateRect(bt->hwnd, NULL, FALSE);
}

void ball_tracker_toggle_visibility(struct ball_tracker *bt)
{
	if (!bt) return;
	pthread_mutex_lock(&bt->mutex);
	if (bt->point_count == 3) {
		bt->enabled = !bt->enabled;
		if (bt->enabled) {
			ball_tracker_start_animation_locked(bt);
		}
	}
	pthread_mutex_unlock(&bt->mutex);
	update_status_text(bt);
	if (bt->hwnd && bt->is_showing)
		InvalidateRect(bt->hwnd, NULL, FALSE);
}

void ball_tracker_cycle_color(struct ball_tracker *bt)
{
	if (!bt) return;
	pthread_mutex_lock(&bt->mutex);
	bt->color = (bt->color + 1) % BALL_TRACKER_COLOR_COUNT;
	pthread_mutex_unlock(&bt->mutex);
	update_status_text(bt);
	if (bt->hwnd && bt->is_showing)
		InvalidateRect(bt->hwnd, NULL, FALSE);
}

void ball_tracker_notify_frame(struct ball_tracker *bt)
{
	if (bt && bt->hwnd && bt->is_showing)
		InvalidateRect(bt->hwnd, NULL, FALSE);
}

bool ball_tracker_is_enabled(struct ball_tracker *bt)
{
	if (!bt) return false;
	return bt->enabled && bt->point_count == 3;
}

bool ball_tracker_is_animating(struct ball_tracker *bt)
{
	if (!bt) return false;
	return bt->enabled && bt->point_count == 3 && bt->is_animating;
}

void ball_tracker_start_animation(struct ball_tracker *bt)
{
	if (!bt) return;
	pthread_mutex_lock(&bt->mutex);
	ball_tracker_start_animation_locked(bt);
	pthread_mutex_unlock(&bt->mutex);
}

#else /* !_WIN32 */

struct ball_tracker *ball_tracker_create(struct replay_source *context) { (void)context; return NULL; }
void ball_tracker_destroy(struct ball_tracker *bt) { (void)bt; }
void ball_tracker_show(struct ball_tracker *bt) { (void)bt; }
void ball_tracker_hide(struct ball_tracker *bt) { (void)bt; }
void ball_tracker_reset_points(struct ball_tracker *bt) { (void)bt; }
void ball_tracker_toggle_visibility(struct ball_tracker *bt) { (void)bt; }
void ball_tracker_cycle_color(struct ball_tracker *bt) { (void)bt; }
void ball_tracker_notify_frame(struct ball_tracker *bt) { (void)bt; }
bool ball_tracker_is_enabled(struct ball_tracker *bt) { (void)bt; return false; }
bool ball_tracker_is_animating(struct ball_tracker *bt) { (void)bt; return false; }
void ball_tracker_start_animation(struct ball_tracker *bt) { (void)bt; }
struct obs_source_frame *ball_tracker_render(struct ball_tracker *bt, struct obs_source_frame *frame) { (void)bt; return frame; }

#endif /* _WIN32 */
