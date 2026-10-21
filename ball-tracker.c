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

#define PREVIEW_WIDTH  550
#define PREVIEW_HEIGHT 310

struct ball_tracker {
	struct replay_source *context;
	pthread_mutex_t mutex;

	/* 3-Point Calibration */
	struct ball_point points[3]; /* 0: Release, 1: Pitch, 2: Stumps */
	int point_count;             /* 0 to 3 */
	bool enabled;
	enum ball_tracker_color color;
	int thickness;               /* 4 to 12 */

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
	enum video_format preview_scaler_format;
	uint8_t *preview_pixels;
	uint32_t preview_linesize;
};

/* ------------------------------------------------------------------ */
/*  YUV / RGB Color Constants                                         */
/* ------------------------------------------------------------------ */
struct color_def {
	uint8_t r, g, b;
	uint8_t y, u, v;
};

static const struct color_def s_colors[] = {
	/* Red: Vivid Cricket Ball Red */
	{245, 40,  50,   85, 108, 240},
	/* Yellow: Glowing Broadcast Gold */
	{255, 230, 20,  208,  22, 158},
	/* Blue: High-Contrast Cyan */
	{30,  180, 255, 138, 202,  60}
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
	const struct color_def *color = &s_colors[bt->color % 3];

	pthread_mutex_unlock(&bt->mutex);

	/* Determine target frame: if frame is already zoomed_frame, write directly.
	 * Otherwise copy to overlay_frame to keep stored replay buffer untouched! */
	struct obs_source_frame *target = frame;
	if (!bt->context || frame != bt->context->zoomed_frame) {
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
		if (bt->overlay_frame) {
			obs_source_frame_copy(bt->overlay_frame, frame);
			target = bt->overlay_frame;
		}
	}

	/* Scale thickness based on video height */
	int radius_core = (int)((float)frame->height / 200.0f);
	if (radius_core < 3) radius_core = 3;
	if (radius_core > 9) radius_core = 9;
	int radius_glow = radius_core + 3;

	/* Calculate quadratic Bézier control points */
	/* Segment 1: Release -> Pitch (natural ballistic arc) */
	struct pt2d c1;
	c1.x = p0.x + (p1.x - p0.x) * 0.48f;
	float arc1 = fabsf(p1.x - p0.x) * 0.08f;
	if (arc1 < 25.0f) arc1 = 25.0f;
	c1.y = ((p0.y < p1.y) ? p0.y : p1.y) - arc1;

	/* Segment 2: Pitch -> Stumps (bounce rebound) */
	struct pt2d c2;
	c2.x = p1.x + (p2.x - p1.x) * 0.45f;
	float arc2 = fabsf(p2.x - p1.x) * 0.05f;
	c2.y = ((p1.y < p2.y) ? p1.y : p2.y) - arc2;

	/* 1. Draw outer glow aura for Segment 1 */
	int steps1 = 50;
	for (int i = 0; i <= steps1; i++) {
		float t = (float)i / (float)steps1;
		struct pt2d pt = eval_bezier2(p0, c1, p1, t);
		draw_filled_circle(target, (int)pt.x, (int)pt.y, radius_glow, color, 65);
	}

	/* 2. Draw outer glow aura for Segment 2 */
	int steps2 = 40;
	for (int i = 0; i <= steps2; i++) {
		float t = (float)i / (float)steps2;
		struct pt2d pt = eval_bezier2(p1, c2, p2, t);
		draw_filled_circle(target, (int)pt.x, (int)pt.y, radius_glow, color, 65);
	}

	/* 3. Draw core solid ribbon for Segment 1 */
	for (int i = 0; i <= steps1; i++) {
		float t = (float)i / (float)steps1;
		struct pt2d pt = eval_bezier2(p0, c1, p1, t);
		draw_filled_circle(target, (int)pt.x, (int)pt.y, radius_core, color, 240);
	}

	/* 4. Draw core solid ribbon for Segment 2 */
	for (int i = 0; i <= steps2; i++) {
		float t = (float)i / (float)steps2;
		struct pt2d pt = eval_bezier2(p1, c2, p2, t);
		draw_filled_circle(target, (int)pt.x, (int)pt.y, radius_core, color, 240);
	}

	/* 5. Draw Point 1: Release Ball Sphere */
	int ball_r = radius_core * 2 + 2;
	draw_filled_circle(target, (int)p0.x, (int)p0.y, ball_r + 2, color, 120);
	draw_filled_circle(target, (int)p0.x, (int)p0.y, ball_r, color, 255);
	draw_filled_circle(target, (int)p0.x - ball_r / 3, (int)p0.y - ball_r / 3, ball_r / 3, &s_white, 200);

	/* 6. Draw Point 2: Pitch Impact Spot */
	draw_filled_circle(target, (int)p1.x, (int)p1.y, ball_r + 4, color, 100);
	draw_filled_circle(target, (int)p1.x, (int)p1.y, ball_r, color, 255);
	draw_filled_circle(target, (int)p1.x, (int)p1.y, ball_r / 2, &s_white, 220);

	/* 7. Draw Point 3: Stumps/End Ball */
	draw_filled_circle(target, (int)p2.x, (int)p2.y, ball_r + 2, color, 120);
	draw_filled_circle(target, (int)p2.x, (int)p2.y, ball_r, color, 255);
	draw_filled_circle(target, (int)p2.x - ball_r / 3, (int)p2.y - ball_r / 3, ball_r / 3, &s_white, 200);

	return target;
}

/* ------------------------------------------------------------------ */
/*  Win32 UI Implementation                                           */
/* ------------------------------------------------------------------ */
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
		const wchar_t *cname = (bt->color == BALL_TRACKER_COLOR_RED)    ? L"Color: Red" :
		                        (bt->color == BALL_TRACKER_COLOR_YELLOW) ? L"Color: Yellow" :
		                                                                   L"Color: Blue";
		SetWindowTextW(bt->hwnd_btn_color, cname);
	}
}

static void trigger_paused_rerender(struct ball_tracker *bt)
{
	if (!bt || !bt->context)
		return;
	struct replay_source *c = bt->context;
	if (!c->play && c->current_replay.video_frame_count) {
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
		}
	}
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

	if (!bt->preview_pixels) {
		bt->preview_linesize = PREVIEW_WIDTH * 4;
		bt->preview_pixels = bzalloc(bt->preview_linesize * PREVIEW_HEIGHT);
	}

	if (!bt->preview_scaler ||
	    bt->preview_scaler_w != src->width ||
	    bt->preview_scaler_h != src->height ||
	    bt->preview_scaler_format != src->format) {
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
		dsi.width = PREVIEW_WIDTH;
		dsi.height = PREVIEW_HEIGHT;
		dsi.range = range;
		dsi.colorspace = cs;

		int ret = video_scaler_create(&bt->preview_scaler, &dsi, &ssi, VIDEO_SCALE_FAST_BILINEAR);
		if (ret != VIDEO_SCALER_SUCCESS)
			return;

		bt->preview_scaler_w = src->width;
		bt->preview_scaler_h = src->height;
		bt->preview_scaler_format = src->format;
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
			15, 10, PREVIEW_WIDTH, 26, hwnd, NULL, NULL, NULL);
		SendMessageA(bt->hwnd_status, WM_SETFONT, (WPARAM)bt->hfont_status, TRUE);

		/* Control buttons Row 1: Scrubbing */
		int btn_y1 = 360;
		int btn_w = 100;
		int btn_h = 28;
		int gap = 12;

		bt->hwnd_btn_prev5 = CreateWindowExW(0, L"BUTTON", L"\u25c4 -5",
			WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
			15, btn_y1, btn_w, btn_h, hwnd, (HMENU)ID_BTN_PREV5, NULL, NULL);
		bt->hwnd_btn_prev1 = CreateWindowExW(0, L"BUTTON", L"\u25c4 -1",
			WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
			15 + (btn_w + gap), btn_y1, btn_w, btn_h, hwnd, (HMENU)ID_BTN_PREV1, NULL, NULL);
		bt->hwnd_btn_play_pause = CreateWindowExW(0, L"BUTTON", L"|| Pause",
			WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
			15 + (btn_w + gap) * 2, btn_y1, btn_w, btn_h, hwnd, (HMENU)ID_BTN_PLAY_PAUSE, NULL, NULL);
		bt->hwnd_btn_next1 = CreateWindowExW(0, L"BUTTON", L"+1 \u25ba",
			WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
			15 + (btn_w + gap) * 3, btn_y1, btn_w, btn_h, hwnd, (HMENU)ID_BTN_NEXT1, NULL, NULL);
		bt->hwnd_btn_next5 = CreateWindowExW(0, L"BUTTON", L"+5 \u25ba",
			WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
			15 + (btn_w + gap) * 4, btn_y1, btn_w, btn_h, hwnd, (HMENU)ID_BTN_NEXT5, NULL, NULL);

		/* Control buttons Row 2: Tracker controls */
		int btn_y2 = 398;
		int bw2 = 125;
		bt->hwnd_btn_reset = CreateWindowExW(0, L"BUTTON", L"\u21bb Reset Points",
			WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
			15, btn_y2, bw2, btn_h, hwnd, (HMENU)ID_BTN_RESET, NULL, NULL);
		bt->hwnd_btn_toggle = CreateWindowExW(0, L"BUTTON", L"Line: ON",
			WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
			15 + bw2 + 10, btn_y2, bw2, btn_h, hwnd, (HMENU)ID_BTN_TOGGLE, NULL, NULL);
		bt->hwnd_btn_color = CreateWindowExW(0, L"BUTTON", L"Color: Red",
			WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
			15 + (bw2 + 10) * 2, btn_y2, bw2, btn_h, hwnd, (HMENU)ID_BTN_COLOR, NULL, NULL);
		bt->hwnd_btn_close = CreateWindowExW(0, L"BUTTON", L"\u2715 Close",
			WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
			15 + (bw2 + 10) * 3, btn_y2, bw2, btn_h, hwnd, (HMENU)ID_BTN_CLOSE, NULL, NULL);

		HWND all_btns[] = {bt->hwnd_btn_prev5, bt->hwnd_btn_prev1, bt->hwnd_btn_play_pause,
		                   bt->hwnd_btn_next1, bt->hwnd_btn_next5, bt->hwnd_btn_reset,
		                   bt->hwnd_btn_toggle, bt->hwnd_btn_color, bt->hwnd_btn_close};
		for (int i = 0; i < 9; i++) {
			SendMessageA(all_btns[i], WM_SETFONT, (WPARAM)bt->hfont_btns, TRUE);
		}

		update_status_text(bt);
		return 0;
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
		int cx = 15;
		int cy = 42;
		if (bt->preview_pixels) {
			BITMAPINFO bmi = {0};
			bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
			bmi.bmiHeader.biWidth = PREVIEW_WIDTH;
			bmi.bmiHeader.biHeight = -PREVIEW_HEIGHT;
			bmi.bmiHeader.biPlanes = 1;
			bmi.bmiHeader.biBitCount = 32;
			bmi.bmiHeader.biCompression = BI_RGB;

			StretchDIBits(hdc, cx, cy, PREVIEW_WIDTH, PREVIEW_HEIGHT,
			              0, 0, PREVIEW_WIDTH, PREVIEW_HEIGHT,
			              bt->preview_pixels, &bmi, DIB_RGB_COLORS, SRCCOPY);
		} else {
			RECT rcBox = {cx, cy, cx + PREVIEW_WIDTH, cy + PREVIEW_HEIGHT};
			FillRect(hdc, &rcBox, bt->hbr_canvas_border);
		}

		/* Draw trajectory line & markers on canvas DC */
		if (bt->point_count >= 1) {
			COLORREF pen_col = (bt->color == BALL_TRACKER_COLOR_RED)    ? RGB(250, 40, 50) :
			                   (bt->color == BALL_TRACKER_COLOR_YELLOW) ? RGB(255, 230, 20) :
			                                                              RGB(30, 180, 255);
			HPEN hPenCurve = CreatePen(PS_SOLID, 4, pen_col);
			HPEN hOldPen = (HPEN)SelectObject(hdc, hPenCurve);

			int px0 = cx + (int)(bt->points[0].u * PREVIEW_WIDTH);
			int py0 = cy + (int)(bt->points[0].v * PREVIEW_HEIGHT);

			if (bt->point_count >= 2) {
				int px1 = cx + (int)(bt->points[1].u * PREVIEW_WIDTH);
				int py1 = cy + (int)(bt->points[1].v * PREVIEW_HEIGHT);

				/* Parabolic Bézier arc 1 */
				struct pt2d c1;
				c1.x = (float)px0 + (float)(px1 - px0) * 0.48f;
				float a1 = fabsf((float)(px1 - px0)) * 0.08f;
				if (a1 < 10.0f) a1 = 10.0f;
				c1.y = ((py0 < py1) ? (float)py0 : (float)py1) - a1;

				struct pt2d start = {(float)px0, (float)py0};
				struct pt2d end1 = {(float)px1, (float)py1};
				MoveToEx(hdc, px0, py0, NULL);
				for (int i = 1; i <= 30; i++) {
					struct pt2d p = eval_bezier2(start, c1, end1, (float)i / 30.0f);
					LineTo(hdc, (int)p.x, (int)p.y);
				}

				if (bt->point_count >= 3) {
					int px2 = cx + (int)(bt->points[2].u * PREVIEW_WIDTH);
					int py2 = cy + (int)(bt->points[2].v * PREVIEW_HEIGHT);

					/* Parabolic Bézier arc 2 */
					struct pt2d c2;
					c2.x = (float)px1 + (float)(px2 - px1) * 0.45f;
					float a2 = fabsf((float)(px2 - px1)) * 0.05f;
					c2.y = ((py1 < py2) ? (float)py1 : (float)py2) - a2;

					struct pt2d end2 = {(float)px2, (float)py2};
					for (int i = 1; i <= 25; i++) {
						struct pt2d p = eval_bezier2(end1, c2, end2, (float)i / 25.0f);
						LineTo(hdc, (int)p.x, (int)p.y);
					}
				}
			}
			SelectObject(hdc, hOldPen);
			DeleteObject(hPenCurve);

			/* Draw point badges: ① Release, ② Pitch, ③ Stumps */
			const char *labels[] = {"1. Release", "2. Pitch", "3. Stumps"};
			COLORREF bcols[] = {RGB(0, 220, 255), RGB(255, 210, 30), RGB(255, 50, 60)};
			SetBkMode(hdc, TRANSPARENT);
			SetTextColor(hdc, RGB(255, 255, 255));

			for (int i = 0; i < bt->point_count; i++) {
				int bx = cx + (int)(bt->points[i].u * PREVIEW_WIDTH);
				int by = cy + (int)(bt->points[i].v * PREVIEW_HEIGHT);
				HBRUSH hbr = CreateSolidBrush(bcols[i]);
				HBRUSH hOldBr = (HBRUSH)SelectObject(hdc, hbr);
				HPEN hpw = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
				HPEN hOldP = (HPEN)SelectObject(hdc, hpw);

				Ellipse(hdc, bx - 7, by - 7, bx + 7, by + 7);
				TextOutA(hdc, bx + 10, by - 8, labels[i], (int)strlen(labels[i]));

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
		int cx = 15;
		int cy = 42;

		if (mx >= cx && mx <= cx + PREVIEW_WIDTH && my >= cy && my <= cy + PREVIEW_HEIGHT) {
			float u = (float)(mx - cx) / (float)PREVIEW_WIDTH;
			float v = (float)(my - cy) / (float)PREVIEW_HEIGHT;
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
			InvalidateRect(hwnd, NULL, FALSE);
			trigger_paused_rerender(bt);
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

	HWND hwnd = CreateWindowExA(
		WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
		"OBSReplayBallTrackerPopup",
		"Cricket Ball Tracker (3-Point)",
		WS_POPUP | WS_CAPTION | WS_SYSMENU,
		CW_USEDEFAULT, CW_USEDEFAULT, 588, 480,
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
	bt->color = BALL_TRACKER_COLOR_RED;
	bt->thickness = 5;
	bt->enabled = false;
	bt->point_count = 0;

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
		ShowWindow(bt->hwnd, SW_SHOW);
		SetWindowPos(bt->hwnd, HWND_TOPMOST, 0, 0, 0, 0,
		             SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
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
	bt->color = (bt->color + 1) % 3;
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

#else /* !_WIN32 */

struct ball_tracker *ball_tracker_create(struct replay_source *context) { (void)context; return NULL; }
void ball_tracker_destroy(struct ball_tracker *bt) { (void)bt; }
void ball_tracker_show(struct ball_tracker *bt) { (void)bt; }
void ball_tracker_hide(struct ball_tracker *bt) { (void)bt; }
void ball_tracker_reset_points(struct ball_tracker *bt) { (void)bt; }
void ball_tracker_toggle_visibility(struct ball_tracker *bt) { (void)bt; }
void ball_tracker_cycle_color(struct ball_tracker *bt) { (void)bt; }
void ball_tracker_notify_frame(struct ball_tracker *bt) { (void)bt; }
struct obs_source_frame *ball_tracker_render(struct ball_tracker *bt, struct obs_source_frame *frame) { (void)bt; return frame; }

#endif /* _WIN32 */
