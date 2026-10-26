/*
 * speed-popup.c
 *
 * Floating "Replay Speed Meter" tool window for OBS Replay Source.
 *
 * Runs in its own Win32 message-pump thread so the popup's UI work
 * (painting, slider drag, button clicks) NEVER touches the OBS render
 * thread.  Communication back to the replay engine uses PostMessage()
 * which is fully asynchronous, keeping the replay path frame-drop free.
 *
 * Public API (declared in speed-popup.h):
 *   speed_popup_create()   - allocate & start the thread
 *   speed_popup_show()     - make the window visible
 *   speed_popup_hide()     - hide without destroying
 *   speed_popup_notify()   - push current speed/direction to the window
 *   speed_popup_destroy()  - tear down and free
 */

#include "speed-popup.h"
#include "replay-source-internal.h"
#include "ball-tracker.h"

#ifdef _WIN32

#include <windows.h>
#include <commctrl.h>
#ifdef _MSC_VER
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#endif

/* ------------------------------------------------------------------ */
/*  Window messages (private to this file)                            */
/* ------------------------------------------------------------------ */
#define WM_USER_UPDATE_ALL  (WM_USER + 101)
#define WM_USER_SHOW_POPUP  (WM_USER + 102)
#define WM_USER_HIDE_POPUP  (WM_USER + 103)

/* ------------------------------------------------------------------ */
/*  Control IDs                                                        */
/* ------------------------------------------------------------------ */
#define ID_SLIDER       2001
#define ID_BTN_MINUS    2002
#define ID_BTN_PLUS     2003
#define ID_BTN_10       2010
#define ID_BTN_25       2025
#define ID_BTN_40       2040
#define ID_BTN_50       2050
#define ID_BTN_60       2060
#define ID_BTN_BACKWARD 2070
#define ID_BTN_FORWARD  2071
#define ID_BTN_PLAY_PAUSE 2072
#define ID_SLIDER_ZOOM       2080
#define ID_BTN_ZOOM_MINUS    2081
#define ID_BTN_ZOOM_PLUS     2082
#define ID_BTN_ZOOM_100      2083
#define ID_BTN_ZOOM_125      2084
#define ID_BTN_ZOOM_150      2085
#define ID_BTN_ZOOM_200      2086
#define ID_BTN_ZOOM_300      2087
#define ID_BTN_BALL_TRACKER  2095

/* ------------------------------------------------------------------ */
/*  Position persistence across show/hide and re-triggers             */
/* ------------------------------------------------------------------ */
static int  s_saved_x       = -1;
static int  s_saved_y       = -1;
static int  s_saved_w       = 450;
static int  s_saved_h       = 430;
static bool s_has_saved_pos = false;

/* ------------------------------------------------------------------ */
/*  Internal struct                                                    */
/* ------------------------------------------------------------------ */
struct speed_popup {
	struct replay_source *context;
	HWND   hwnd;
	HWND   hwnd_slider;
	HWND   hwnd_label;
	HWND   hwnd_minus;
	HWND   hwnd_plus;
	HWND   hwnd_b10;
	HWND   hwnd_b25;
	HWND   hwnd_b40;
	HWND   hwnd_b50;
	HWND   hwnd_b60;
	HWND   hwnd_backward;
	HWND   hwnd_forward;
	HWND   hwnd_play_pause;
	HWND   hwnd_zoom_label;
	HWND   hwnd_slider_zoom;
	HWND   hwnd_zoom_minus;
	HWND   hwnd_zoom_plus;
	HWND   hwnd_zoom_100;
	HWND   hwnd_zoom_125;
	HWND   hwnd_zoom_150;
	HWND   hwnd_zoom_200;
	HWND   hwnd_zoom_300;
	HWND   hwnd_ball_tracker;
	HFONT  hfont_title;
	HFONT  hfont_btns;
	HBRUSH hbr_bg;
	HANDLE thread;
	DWORD  thread_id;
	bool   is_showing;
	int    saved_x;
	int    saved_y;
	int    saved_w;
	int    saved_h;
	bool   has_saved_pos;
};

/* ------------------------------------------------------------------ */
/*  UI helpers                                                         */
/* ------------------------------------------------------------------ */

static void popup_refresh_label(struct speed_popup *p)
{
	char buf[96];
	bool bwd = p->context->backward;
	float spd = p->context->speed_percent;
	if (bwd)
		snprintf(buf, sizeof(buf), "\u25c4 Speed: %.0f%%  [Backward]", spd);
	else
		snprintf(buf, sizeof(buf), "\u25ba Speed: %.0f%%", spd);
	SetWindowTextA(p->hwnd_label, buf);
}

static void popup_refresh_dir_buttons(struct speed_popup *p)
{
	bool bwd = p->context->backward;
	EnableWindow(p->hwnd_backward, bwd ? FALSE : TRUE);
	EnableWindow(p->hwnd_forward,  bwd ? TRUE  : FALSE);
}

static void popup_refresh_play_pause_button(struct speed_popup *p)
{
	if (!p || !p->hwnd_play_pause || !p->context)
		return;
	bool is_playing = p->context->play && !p->context->end;
	SetWindowTextW(p->hwnd_play_pause, is_playing ? L"||  Pause" : L"\u25BA  Play");
}

static void popup_refresh_zoom_label(struct speed_popup *p)
{
	if (!p || !p->hwnd_zoom_label || !p->context)
		return;
	char buf[96];
	float zm = p->context->zoom_percent;
	if (zm <= 100.5f)
		snprintf(buf, sizeof(buf), "Zoom: 1.0x (100%% - Normal)");
	else
		snprintf(buf, sizeof(buf), "Zoom: %.2fx (%.0f%%)", zm / 100.0f, zm);
	SetWindowTextA(p->hwnd_zoom_label, buf);
}

static void popup_layout(struct speed_popup *p, HWND hwnd)
{
	if (!p || !p->hwnd_label)
		return;

	RECT rc;
	GetClientRect(hwnd, &rc);
	int cw  = rc.right - rc.left;
	int ch  = rc.bottom - rc.top;
	int pad = 12;

	if (cw <= 0 || ch <= 0)
		return;

	int area = cw - pad * 2;
	if (area < 10) area = 10;

	/* Speed section */
	int label_y   = pad;
	int label_h   = 24;
	int slider_y  = label_y + label_h + 2;
	int slider_h  = 28;
	int btn_h     = 26;
	int btn_gap   = 6;

	int spdbtn_y  = slider_y + slider_h + btn_gap;
	int dirbtn_y  = spdbtn_y + btn_h + btn_gap;
	int playbtn_y = dirbtn_y + btn_h + btn_gap;

	MoveWindow(p->hwnd_label,  pad,     label_y,  cw - pad * 2,       label_h,  TRUE);
	MoveWindow(p->hwnd_slider, pad - 2, slider_y, cw - (pad - 2) * 2, slider_h, TRUE);

	int bws = area / 8;
	int bwm = (area - bws * 2) / 5;
	HWND sb[7] = {p->hwnd_minus, p->hwnd_b10, p->hwnd_b25,
	              p->hwnd_b40,   p->hwnd_b50, p->hwnd_b60, p->hwnd_plus};
	int bx = pad;
	for (int i = 0; i < 7; i++) {
		int bw = (i == 0 || i == 6) ? bws : bwm;
		MoveWindow(sb[i], bx, spdbtn_y, bw, btn_h, TRUE);
		bx += bw + 2;
	}

	int dh = (area - 6) / 2;
	MoveWindow(p->hwnd_backward, pad,          dirbtn_y, dh, btn_h, TRUE);
	MoveWindow(p->hwnd_forward,  pad + dh + 6, dirbtn_y, dh, btn_h, TRUE);

	if (p->hwnd_play_pause) {
		int play_w = dh;
		int play_x = pad + (area - play_w) / 2;
		MoveWindow(p->hwnd_play_pause, play_x, playbtn_y, play_w, btn_h, TRUE);
	}

	/* Zoom section */
	if (p->hwnd_zoom_label && p->hwnd_slider_zoom) {
		int zlabel_y  = playbtn_y + btn_h + 10;
		int zlabel_h  = 22;
		int zslider_y = zlabel_y + zlabel_h + 2;
		int zslider_h = 28;
		int zbtn_y    = zslider_y + zslider_h + btn_gap;

		MoveWindow(p->hwnd_zoom_label,  pad,     zlabel_y,  cw - pad * 2,       zlabel_h,  TRUE);
		MoveWindow(p->hwnd_slider_zoom, pad - 2, zslider_y, cw - (pad - 2) * 2, zslider_h, TRUE);

		HWND zb[7] = {p->hwnd_zoom_minus, p->hwnd_zoom_100, p->hwnd_zoom_125,
		              p->hwnd_zoom_150,   p->hwnd_zoom_200, p->hwnd_zoom_300, p->hwnd_zoom_plus};
		int zbx = pad;
		for (int i = 0; i < 7; i++) {
			int bw = (i == 0 || i == 6) ? bws : bwm;
			MoveWindow(zb[i], zbx, zbtn_y, bw, btn_h, TRUE);
			zbx += bw + 2;
		}

		if (p->hwnd_ball_tracker) {
			int tracker_y = zbtn_y + btn_h + 8;
			MoveWindow(p->hwnd_ball_tracker, pad, tracker_y, cw - pad * 2, btn_h, TRUE);
		}
	}

	InvalidateRect(hwnd, NULL, TRUE);
}

/* ------------------------------------------------------------------ */
/*  Window procedure                                                   */
/* ------------------------------------------------------------------ */
static LRESULT CALLBACK SpeedPopupWndProc(HWND hwnd, UINT msg,
                                          WPARAM wParam, LPARAM lParam)
{
	struct speed_popup *p =
		(struct speed_popup *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);

	switch (msg) {

	case WM_CREATE: {
		CREATESTRUCTA *cs = (CREATESTRUCTA *)lParam;
		p = (struct speed_popup *)cs->lpCreateParams;
		SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)p);
		p->hwnd = hwnd;

		p->hbr_bg = CreateSolidBrush(RGB(24, 26, 32));
		p->hfont_title = CreateFontA(20, 0, 0, 0, FW_BOLD, 0, 0, 0,
			DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
			DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
		p->hfont_btns = CreateFontA(13, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
			DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
			DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");

		if (s_has_saved_pos) {
			p->saved_x = s_saved_x;
			p->saved_y = s_saved_y;
			p->saved_w = s_saved_w;
			p->saved_h = s_saved_h;
			p->has_saved_pos = true;
		}

		p->hwnd_label = CreateWindowExA(0, "STATIC", "",
			WS_CHILD | WS_VISIBLE | SS_LEFT,
			0, 0, 10, 10, hwnd, NULL, NULL, NULL);
		SendMessageA(p->hwnd_label, WM_SETFONT, (WPARAM)p->hfont_title, TRUE);

		INITCOMMONCONTROLSEX icex = {sizeof(icex), ICC_BAR_CLASSES};
		InitCommonControlsEx(&icex);
		p->hwnd_slider = CreateWindowExA(0, TRACKBAR_CLASSA, "",
			WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_NOTICKS,
			0, 0, 10, 10, hwnd, (HMENU)ID_SLIDER, NULL, NULL);
		SendMessageA(p->hwnd_slider, TBM_SETRANGE, TRUE,
			MAKELONG((int)SETTING_SPEED_MIN, (int)SETTING_SPEED_MAX));
		int cur = (int)p->context->speed_percent;
		if (cur < (int)SETTING_SPEED_MIN) cur = (int)SETTING_SPEED_MIN;
		if (cur > (int)SETTING_SPEED_MAX) cur = (int)SETTING_SPEED_MAX;
		SendMessageA(p->hwnd_slider, TBM_SETPOS, TRUE, cur);

		/* Speed buttons */
		struct { HWND *h; const char *txt; int id; } sb[] = {
			{&p->hwnd_minus,"-",  ID_BTN_MINUS},
			{&p->hwnd_b10,  "10%",ID_BTN_10},
			{&p->hwnd_b25,  "25%",ID_BTN_25},
			{&p->hwnd_b40,  "40%",ID_BTN_40},
			{&p->hwnd_b50,  "50%",ID_BTN_50},
			{&p->hwnd_b60,  "60%",ID_BTN_60},
			{&p->hwnd_plus, "+",  ID_BTN_PLUS},
		};
		for (int i = 0; i < 7; i++) {
			*sb[i].h = CreateWindowExA(0, "BUTTON", sb[i].txt,
				WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
				0, 0, 10, 10, hwnd, (HMENU)(intptr_t)sb[i].id, NULL, NULL);
			SendMessageA(*sb[i].h, WM_SETFONT, (WPARAM)p->hfont_btns, TRUE);
		}

		/* Direction buttons */
		p->hwnd_backward = CreateWindowExA(0, "BUTTON", "\u25c4  Backward",
			WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
			0, 0, 10, 10, hwnd, (HMENU)ID_BTN_BACKWARD, NULL, NULL);
		p->hwnd_forward = CreateWindowExA(0, "BUTTON", "Forward  \u25ba",
			WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
			0, 0, 10, 10, hwnd, (HMENU)ID_BTN_FORWARD, NULL, NULL);
		SendMessageA(p->hwnd_backward, WM_SETFONT, (WPARAM)p->hfont_btns, TRUE);
		SendMessageA(p->hwnd_forward,  WM_SETFONT, (WPARAM)p->hfont_btns, TRUE);

		/* Play / Pause toggle button (centered below Backward / Forward) */
		p->hwnd_play_pause = CreateWindowExW(0, L"BUTTON", L"",
			WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
			0, 0, 10, 10, hwnd, (HMENU)ID_BTN_PLAY_PAUSE, NULL, NULL);
		SendMessageA(p->hwnd_play_pause, WM_SETFONT, (WPARAM)p->hfont_btns, TRUE);

		/* Zoom label and slider */
		p->hwnd_zoom_label = CreateWindowExA(0, "STATIC", "",
			WS_CHILD | WS_VISIBLE | SS_LEFT,
			0, 0, 10, 10, hwnd, NULL, NULL, NULL);
		SendMessageA(p->hwnd_zoom_label, WM_SETFONT, (WPARAM)p->hfont_btns, TRUE);

		p->hwnd_slider_zoom = CreateWindowExA(0, TRACKBAR_CLASSA, "",
			WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_NOTICKS,
			0, 0, 10, 10, hwnd, (HMENU)ID_SLIDER_ZOOM, NULL, NULL);
		SendMessageA(p->hwnd_slider_zoom, TBM_SETRANGE, TRUE,
			MAKELONG((int)SETTING_ZOOM_MIN, (int)SETTING_ZOOM_MAX));
		int cur_zm = (int)p->context->zoom_percent;
		if (cur_zm < (int)SETTING_ZOOM_MIN) cur_zm = (int)SETTING_ZOOM_MIN;
		if (cur_zm > (int)SETTING_ZOOM_MAX) cur_zm = (int)SETTING_ZOOM_MAX;
		SendMessageA(p->hwnd_slider_zoom, TBM_SETPOS, TRUE, cur_zm);

		/* Zoom quick preset buttons */
		struct { HWND *h; const char *txt; int id; } zb[] = {
			{&p->hwnd_zoom_minus, "-",     ID_BTN_ZOOM_MINUS},
			{&p->hwnd_zoom_100,   "1.0x",  ID_BTN_ZOOM_100},
			{&p->hwnd_zoom_125,   "1.25x", ID_BTN_ZOOM_125},
			{&p->hwnd_zoom_150,   "1.5x",  ID_BTN_ZOOM_150},
			{&p->hwnd_zoom_200,   "2.0x",  ID_BTN_ZOOM_200},
			{&p->hwnd_zoom_300,   "3.0x",  ID_BTN_ZOOM_300},
			{&p->hwnd_zoom_plus,  "+",     ID_BTN_ZOOM_PLUS},
		};
		for (int i = 0; i < 7; i++) {
			*zb[i].h = CreateWindowExA(0, "BUTTON", zb[i].txt,
				WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
				0, 0, 10, 10, hwnd, (HMENU)(intptr_t)zb[i].id, NULL, NULL);
			SendMessageA(*zb[i].h, WM_SETFONT, (WPARAM)p->hfont_btns, TRUE);
		}

		p->hwnd_ball_tracker = CreateWindowExW(0, L"BUTTON", L"\u25cf  Cricket Ball Tracker (3-Point)",
			WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
			0, 0, 10, 10, hwnd, (HMENU)ID_BTN_BALL_TRACKER, NULL, NULL);
		SendMessageA(p->hwnd_ball_tracker, WM_SETFONT, (WPARAM)p->hfont_btns, TRUE);

		popup_refresh_label(p);
		popup_refresh_dir_buttons(p);
		popup_refresh_play_pause_button(p);
		popup_refresh_zoom_label(p);
		return 0;
	}

	case WM_ERASEBKGND: {
		if (p && p->hbr_bg) {
			HDC hdc = (HDC)wParam;
			RECT rc;
			GetClientRect(hwnd, &rc);
			FillRect(hdc, &rc, p->hbr_bg);
			return 1;
		}
		return 0;
	}

	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC hdc = BeginPaint(hwnd, &ps);
		if (p && p->hbr_bg) {
			FillRect(hdc, &ps.rcPaint, p->hbr_bg);
		}
		EndPaint(hwnd, &ps);
		return 0;
	}

	case WM_CTLCOLORSTATIC: {
		HDC hdc = (HDC)wParam;
		HWND hCtl = (HWND)lParam;
		if (p && hCtl == p->hwnd_zoom_label) {
			SetTextColor(hdc, RGB(255, 215, 0));
		} else {
			SetTextColor(hdc, RGB(0, 215, 255));
		}
		SetBkColor(hdc, RGB(24, 26, 32));
		return (LRESULT)p->hbr_bg;
	}

	case WM_CTLCOLORDLG:
	case WM_CTLCOLORBTN:
		return (LRESULT)p->hbr_bg;

	case WM_HSCROLL: {
		if (p && p->hwnd_slider && (HWND)lParam == p->hwnd_slider) {
			int pos = (int)SendMessageA(p->hwnd_slider, TBM_GETPOS, 0, 0);
			if (pos < (int)SETTING_SPEED_MIN) pos = (int)SETTING_SPEED_MIN;
			if (pos > (int)SETTING_SPEED_MAX) pos = (int)SETTING_SPEED_MAX;
			update_speed(p->context, (float)pos);
			popup_refresh_label(p);
		} else if (p && p->hwnd_slider_zoom && (HWND)lParam == p->hwnd_slider_zoom) {
			int pos = (int)SendMessageA(p->hwnd_slider_zoom, TBM_GETPOS, 0, 0);
			if (pos < (int)SETTING_ZOOM_MIN) pos = (int)SETTING_ZOOM_MIN;
			if (pos > (int)SETTING_ZOOM_MAX) pos = (int)SETTING_ZOOM_MAX;
			update_zoom(p->context, (float)pos);
			popup_refresh_zoom_label(p);
		}
		return 0;
	}

	case WM_COMMAND: {
		if (!p) return 0;
		int id = LOWORD(wParam);
		float spd = p->context->speed_percent;
		bool chg = false;
		float zm = p->context->zoom_percent;
		bool zm_chg = false;

		switch (id) {
		case ID_BTN_MINUS:   spd -= 5.0f; chg = true; break;
		case ID_BTN_PLUS:    spd += 5.0f; chg = true; break;
		case ID_BTN_10:      spd = 10.f;  chg = true; break;
		case ID_BTN_25:      spd = 25.f;  chg = true; break;
		case ID_BTN_40:      spd = 40.f;  chg = true; break;
		case ID_BTN_50:      spd = 50.f;  chg = true; break;
		case ID_BTN_60:      spd = 60.f;  chg = true; break;
		case ID_BTN_BACKWARD:
			replay_set_backward(p->context);
			popup_refresh_label(p);
			popup_refresh_dir_buttons(p);
			break;
		case ID_BTN_FORWARD:
			replay_set_forward(p->context);
			popup_refresh_label(p);
			popup_refresh_dir_buttons(p);
			break;
		case ID_BTN_PLAY_PAUSE:
			replay_toggle_play_pause(p->context);
			popup_refresh_play_pause_button(p);
			break;
		case ID_BTN_ZOOM_MINUS: zm -= 10.0f; zm_chg = true; break;
		case ID_BTN_ZOOM_PLUS:  zm += 10.0f; zm_chg = true; break;
		case ID_BTN_ZOOM_100:   zm = 100.0f; zm_chg = true; break;
		case ID_BTN_ZOOM_125:   zm = 125.0f; zm_chg = true; break;
		case ID_BTN_ZOOM_150:   zm = 150.0f; zm_chg = true; break;
		case ID_BTN_ZOOM_200:   zm = 200.0f; zm_chg = true; break;
		case ID_BTN_ZOOM_300:   zm = 300.0f; zm_chg = true; break;
		case ID_BTN_BALL_TRACKER:
			if (p->context && p->context->ball_tracker)
				ball_tracker_show(p->context->ball_tracker);
			break;
		}
		if (chg) {
			if (spd < SETTING_SPEED_MIN) spd = SETTING_SPEED_MIN;
			if (spd > SETTING_SPEED_MAX) spd = SETTING_SPEED_MAX;
			update_speed(p->context, spd);
			SendMessageA(p->hwnd_slider, TBM_SETPOS, TRUE, (LPARAM)(int)spd);
			popup_refresh_label(p);
		}
		if (zm_chg) {
			if (zm < SETTING_ZOOM_MIN) zm = SETTING_ZOOM_MIN;
			if (zm > SETTING_ZOOM_MAX) zm = SETTING_ZOOM_MAX;
			update_zoom(p->context, zm);
			SendMessageA(p->hwnd_slider_zoom, TBM_SETPOS, TRUE, (LPARAM)(int)zm);
			popup_refresh_zoom_label(p);
		}
		return 0;
	}

	case WM_WINDOWPOSCHANGED: {
		WINDOWPOS *wp = (WINDOWPOS *)lParam;
		if (p && (!(wp->flags & SWP_NOMOVE) || !(wp->flags & SWP_NOSIZE))) {
			if (!IsIconic(hwnd) && IsWindowVisible(hwnd)) {
				RECT wr;
				GetWindowRect(hwnd, &wr);
				p->saved_x = wr.left;
				p->saved_y = wr.top;
				p->saved_w = wr.right - wr.left;
				p->saved_h = wr.bottom - wr.top;
				p->has_saved_pos = true;
				s_saved_x = p->saved_x;
				s_saved_y = p->saved_y;
				s_saved_w = p->saved_w;
				s_saved_h = p->saved_h;
				s_has_saved_pos = true;
			}
		}
		return DefWindowProcA(hwnd, msg, wParam, lParam);
	}

	case WM_SIZE:
		if (p) {
			popup_layout(p, hwnd);
		}
		return 0;

	case WM_GETMINMAXINFO: {
		MINMAXINFO *mmi = (MINMAXINFO *)lParam;
		mmi->ptMinTrackSize.x = 360;
		mmi->ptMinTrackSize.y = 390;
		return 0;
	}

	case WM_USER_SHOW_POPUP: {
		if (p && p->hwnd) {
			int sx, sy, sw, sh;
			if (p->has_saved_pos || s_has_saved_pos) {
				sx = p->has_saved_pos ? p->saved_x : s_saved_x;
				sy = p->has_saved_pos ? p->saved_y : s_saved_y;
				sw = p->has_saved_pos ? p->saved_w : s_saved_w;
				sh = p->has_saved_pos ? p->saved_h : s_saved_h;
				POINT pt = {sx + 50, sy + 50};
				HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONULL);
				if (!hMon) {
					RECT rcWork;
					SystemParametersInfoA(SPI_GETWORKAREA, 0, &rcWork, 0);
					sx = rcWork.right  - sw - 24;
					sy = rcWork.bottom - sh - 48;
				}
			} else {
				sw = 450;
				sh = 430;
				RECT rcWork;
				SystemParametersInfoA(SPI_GETWORKAREA, 0, &rcWork, 0);
				sx = rcWork.right  - sw - 24;
				sy = rcWork.bottom - sh - 48;
				p->saved_x = sx;
				p->saved_y = sy;
				p->saved_w = sw;
				p->saved_h = sh;
				p->has_saved_pos = true;
				s_saved_x = sx;
				s_saved_y = sy;
				s_saved_w = sw;
				s_saved_h = sh;
				s_has_saved_pos = true;
			}
			SetWindowPos(p->hwnd, HWND_TOPMOST, sx, sy, sw, sh,
			             SWP_SHOWWINDOW | SWP_NOACTIVATE);
			p->is_showing = true;
			popup_layout(p, hwnd);
			InvalidateRect(p->hwnd, NULL, TRUE);
			UpdateWindow(p->hwnd);
			SendMessageA(hwnd, WM_USER_UPDATE_ALL, 0, 0);
		}
		return 0;
	}

	case WM_USER_HIDE_POPUP:
		if (p && p->hwnd) {
			ShowWindow(p->hwnd, SW_HIDE);
			p->is_showing = false;
		}
		return 0;

	case WM_USER_UPDATE_ALL: {
		if (p && p->hwnd_slider && p->hwnd_label) {
			int pos = (int)p->context->speed_percent;
			if (pos < (int)SETTING_SPEED_MIN) pos = (int)SETTING_SPEED_MIN;
			if (pos > (int)SETTING_SPEED_MAX) pos = (int)SETTING_SPEED_MAX;
			SendMessageA(p->hwnd_slider, TBM_SETPOS, TRUE, pos);
			popup_refresh_label(p);
			popup_refresh_dir_buttons(p);
			popup_refresh_play_pause_button(p);
		}
		if (p && p->hwnd_slider_zoom && p->hwnd_zoom_label) {
			int zpos = (int)p->context->zoom_percent;
			if (zpos < (int)SETTING_ZOOM_MIN) zpos = (int)SETTING_ZOOM_MIN;
			if (zpos > (int)SETTING_ZOOM_MAX) zpos = (int)SETTING_ZOOM_MAX;
			SendMessageA(p->hwnd_slider_zoom, TBM_SETPOS, TRUE, zpos);
			popup_refresh_zoom_label(p);
		}
		return 0;
	}

	case WM_CLOSE:
		if (p) { ShowWindow(hwnd, SW_HIDE); p->is_showing = false; }
		return 0;

	case WM_DESTROY:
		if (p) {
			if (p->hfont_title) DeleteObject(p->hfont_title);
			if (p->hfont_btns)  DeleteObject(p->hfont_btns);
			if (p->hbr_bg)      DeleteObject(p->hbr_bg);
		}
		PostQuitMessage(0);
		return 0;
	}

	return DefWindowProcA(hwnd, msg, wParam, lParam);
}

/* ------------------------------------------------------------------ */
/*  Background thread – owns the message pump                         */
/* ------------------------------------------------------------------ */
static DWORD WINAPI SpeedPopupThread(LPVOID param)
{
	struct speed_popup *p = (struct speed_popup *)param;
	WNDCLASSA wc = {0};
	MSG msg;

	wc.style         = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc   = SpeedPopupWndProc;
	wc.hInstance     = GetModuleHandle(NULL);
	wc.lpszClassName = "OBSReplaySpeedMeterPopup";
	wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = NULL;
	RegisterClassA(&wc);

	HWND hwnd = CreateWindowExA(
		WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
		"OBSReplaySpeedMeterPopup",
		"Replay Speed Meter",
		WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_SIZEBOX,
		CW_USEDEFAULT, CW_USEDEFAULT, 450, 430,
		NULL, NULL, GetModuleHandle(NULL), p);

	if (!hwnd) {
		UnregisterClassA("OBSReplaySpeedMeterPopup", GetModuleHandle(NULL));
		return 0;
	}

	while (GetMessageA(&msg, NULL, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessageA(&msg);
	}

	UnregisterClassA("OBSReplaySpeedMeterPopup", GetModuleHandle(NULL));
	return 0;
}

/* ================================================================== */
/*  Public API                                                         */
/* ================================================================== */

void speed_popup_create(struct replay_source *context)
{
	if (context->speed_popup)
		return;
	struct speed_popup *p = bzalloc(sizeof(*p));
	p->context           = context;
	context->speed_popup = p;
	p->thread = CreateThread(NULL, 0, SpeedPopupThread, p, 0, &p->thread_id);
	os_sleep_ms(25);
}

void speed_popup_show(struct replay_source *context)
{
	if (!context->speed_popup)
		speed_popup_create(context);
	struct speed_popup *p = (struct speed_popup *)context->speed_popup;
	if (!p) return;
	for (int i = 0; !p->hwnd && i < 20; i++)
		os_sleep_ms(10);
	if (p->hwnd)
		PostMessageA(p->hwnd, WM_USER_SHOW_POPUP, 0, 0);
}

void speed_popup_hide(struct replay_source *context)
{
	if (!context) return;
	if (context->speed_popup_always_show)
		return;
	struct speed_popup *p = (struct speed_popup *)context->speed_popup;
	if (p && p->hwnd)
		PostMessageA(p->hwnd, WM_USER_HIDE_POPUP, 0, 0);
}

void speed_popup_notify(struct replay_source *context)
{
	struct speed_popup *p = (struct speed_popup *)context->speed_popup;
	if (p && p->hwnd && p->is_showing)
		PostMessageA(p->hwnd, WM_USER_UPDATE_ALL, 0, 0);
}

void speed_popup_destroy(struct replay_source *context)
{
	struct speed_popup *p = (struct speed_popup *)context->speed_popup;
	if (!p) return;
	if (p->hwnd)
		PostMessageA(p->hwnd, WM_DESTROY, 0, 0);
	if (p->thread) {
		WaitForSingleObject(p->thread, 2000);
		CloseHandle(p->thread);
	}
	bfree(p);
	context->speed_popup = NULL;
}

#else /* !_WIN32 */

void speed_popup_create(struct replay_source *context) { (void)context; }
void speed_popup_show(struct replay_source *context) { (void)context; }
void speed_popup_hide(struct replay_source *context) { (void)context; }
void speed_popup_notify(struct replay_source *context) { (void)context; }
void speed_popup_destroy(struct replay_source *context) { (void)context; }

#endif /* _WIN32 */
