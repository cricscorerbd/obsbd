/*
 * replay-save.c
 *
 * Writes a replay save job (video frames + real replay audio) to an MP4
 * file with the Windows Media Foundation sink writer (H.264 + AAC).
 *
 * Unlike an OBS output this is not driven by OBS's real-time audio/video
 * pipeline, so a replay is encoded as fast as the encoder allows on the
 * save worker thread.  Media Foundation picks a hardware H.264 encoder
 * (NVENC / QuickSync / AMF) when one is available and falls back to the
 * Microsoft software encoder otherwise.  It ships with Windows, so the
 * plugin does not depend on the FFmpeg DLL version of the installed OBS.
 */

#include "replay-save.h"
#include "replay-source-internal.h"

#ifdef _WIN32

#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <initguid.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <media-io/video-scaler.h>

#define HNS_PER_SEC 10000000LL

struct mf_save {
	IMFSinkWriter *writer;
	DWORD video_stream;
	DWORD audio_stream;
	bool has_audio;

	uint32_t width;
	uint32_t height;
	enum video_colorspace cs;
	video_scaler_t *scaler;
	enum video_format scaler_format;
	uint32_t scaler_width;
	uint32_t scaler_height;
	bool scaler_full_range;
	LONGLONG last_video_time;
	LONGLONG frame_duration;

	int channels;
	uint32_t rate;
	int64_t next_sample; /* audio timeline position, in samples since the first video frame */
	int64_t end_sample;  /* audio past the last video frame is dropped */
	uint64_t packet_index;
};

static int64_t ns_to_samples(int64_t ns, uint32_t rate)
{
	return ns * (int64_t)rate / 1000000000LL;
}

static HRESULT set_pair(IMFMediaType *t, REFGUID key, UINT32 hi, UINT32 lo)
{
	return IMFMediaType_SetUINT64(t, key, ((UINT64)hi << 32) | lo);
}

static HRESULT set_video_attrs(IMFMediaType *t, const struct mf_save *s, const struct replay_save_job *job)
{
	HRESULT hr = IMFMediaType_SetGUID(t, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
	if (SUCCEEDED(hr))
		hr = IMFMediaType_SetUINT32(t, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
	if (SUCCEEDED(hr))
		hr = set_pair(t, &MF_MT_FRAME_SIZE, s->width, s->height);
	if (SUCCEEDED(hr))
		hr = set_pair(t, &MF_MT_FRAME_RATE, job->fps_num, job->fps_den);
	if (SUCCEEDED(hr))
		hr = set_pair(t, &MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
	if (SUCCEEDED(hr))
		hr = IMFMediaType_SetUINT32(t, &MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
	if (SUCCEEDED(hr))
		hr = IMFMediaType_SetUINT32(t, &MF_MT_YUV_MATRIX,
					    s->cs == VIDEO_CS_601 ? MFVideoTransferMatrix_BT601
								  : MFVideoTransferMatrix_BT709);
	return hr;
}

static HRESULT add_video_stream(struct mf_save *s, const struct replay_save_job *job)
{
	IMFMediaType *out = NULL;
	IMFMediaType *in = NULL;
	const double fps = (double)job->fps_num / (double)job->fps_den;
	double bitrate = (double)s->width * (double)s->height * fps * 0.1;
	if (bitrate < 2000000.0)
		bitrate = 2000000.0;
	if (bitrate > 50000000.0)
		bitrate = 50000000.0;

	HRESULT hr = MFCreateMediaType(&out);
	if (SUCCEEDED(hr))
		hr = set_video_attrs(out, s, job);
	if (SUCCEEDED(hr))
		hr = IMFMediaType_SetGUID(out, &MF_MT_SUBTYPE, &MFVideoFormat_H264);
	if (SUCCEEDED(hr))
		hr = IMFMediaType_SetUINT32(out, &MF_MT_AVG_BITRATE, (UINT32)bitrate);
	if (SUCCEEDED(hr))
		hr = IMFMediaType_SetUINT32(out, &MF_MT_MPEG2_PROFILE, 100 /* eAVEncH264VProfile_High */);
	if (SUCCEEDED(hr))
		hr = IMFSinkWriter_AddStream(s->writer, out, &s->video_stream);

	if (SUCCEEDED(hr))
		hr = MFCreateMediaType(&in);
	if (SUCCEEDED(hr))
		hr = set_video_attrs(in, s, job);
	if (SUCCEEDED(hr))
		hr = IMFMediaType_SetGUID(in, &MF_MT_SUBTYPE, &MFVideoFormat_NV12);
	if (SUCCEEDED(hr))
		hr = IMFMediaType_SetUINT32(in, &MF_MT_DEFAULT_STRIDE, s->width);
	if (SUCCEEDED(hr))
		hr = IMFSinkWriter_SetInputMediaType(s->writer, s->video_stream, in, NULL);

	if (out)
		IMFMediaType_Release(out);
	if (in)
		IMFMediaType_Release(in);
	return hr;
}

static HRESULT add_audio_stream(struct mf_save *s)
{
	IMFMediaType *out = NULL;
	IMFMediaType *in = NULL;
	const UINT32 block_align = (UINT32)s->channels * sizeof(int16_t);

	HRESULT hr = MFCreateMediaType(&out);
	if (SUCCEEDED(hr))
		hr = IMFMediaType_SetGUID(out, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
	if (SUCCEEDED(hr))
		hr = IMFMediaType_SetGUID(out, &MF_MT_SUBTYPE, &MFAudioFormat_AAC);
	if (SUCCEEDED(hr))
		hr = IMFMediaType_SetUINT32(out, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
	if (SUCCEEDED(hr))
		hr = IMFMediaType_SetUINT32(out, &MF_MT_AUDIO_SAMPLES_PER_SECOND, s->rate);
	if (SUCCEEDED(hr))
		hr = IMFMediaType_SetUINT32(out, &MF_MT_AUDIO_NUM_CHANNELS, (UINT32)s->channels);
	if (SUCCEEDED(hr)) /* 192 kbps; the Microsoft AAC encoder accepts 12000/16000/20000/24000 */
		hr = IMFMediaType_SetUINT32(out, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);
	if (SUCCEEDED(hr))
		hr = IMFSinkWriter_AddStream(s->writer, out, &s->audio_stream);

	if (SUCCEEDED(hr))
		hr = MFCreateMediaType(&in);
	if (SUCCEEDED(hr))
		hr = IMFMediaType_SetGUID(in, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
	if (SUCCEEDED(hr))
		hr = IMFMediaType_SetGUID(in, &MF_MT_SUBTYPE, &MFAudioFormat_PCM);
	if (SUCCEEDED(hr))
		hr = IMFMediaType_SetUINT32(in, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
	if (SUCCEEDED(hr))
		hr = IMFMediaType_SetUINT32(in, &MF_MT_AUDIO_SAMPLES_PER_SECOND, s->rate);
	if (SUCCEEDED(hr))
		hr = IMFMediaType_SetUINT32(in, &MF_MT_AUDIO_NUM_CHANNELS, (UINT32)s->channels);
	if (SUCCEEDED(hr))
		hr = IMFMediaType_SetUINT32(in, &MF_MT_AUDIO_BLOCK_ALIGNMENT, block_align);
	if (SUCCEEDED(hr))
		hr = IMFMediaType_SetUINT32(in, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, s->rate * block_align);
	if (SUCCEEDED(hr))
		hr = IMFSinkWriter_SetInputMediaType(s->writer, s->audio_stream, in, NULL);

	if (out)
		IMFMediaType_Release(out);
	if (in)
		IMFMediaType_Release(in);
	return hr;
}

static void close_writer(struct mf_save *s)
{
	if (s->writer)
		IMFSinkWriter_Release(s->writer);
	s->writer = NULL;
}

static HRESULT open_writer(struct mf_save *s, const struct replay_save_job *job, const wchar_t *path, bool hardware,
			   bool audio)
{
	IMFAttributes *attrs = NULL;
	HRESULT hr = MFCreateAttributes(&attrs, 2);
	if (SUCCEEDED(hr))
		hr = IMFAttributes_SetUINT32(attrs, &MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, hardware ? TRUE : FALSE);
	if (SUCCEEDED(hr))
		hr = IMFAttributes_SetGUID(attrs, &MF_TRANSCODE_CONTAINERTYPE, &MFTranscodeContainerType_MPEG4);
	if (SUCCEEDED(hr))
		hr = MFCreateSinkWriterFromURL(path, NULL, attrs, &s->writer);
	if (attrs)
		IMFAttributes_Release(attrs);

	if (SUCCEEDED(hr))
		hr = add_video_stream(s, job);
	if (SUCCEEDED(hr) && audio)
		hr = add_audio_stream(s);
	if (SUCCEEDED(hr))
		hr = IMFSinkWriter_BeginWriting(s->writer);

	s->has_audio = SUCCEEDED(hr) && audio;
	if (FAILED(hr))
		close_writer(s);
	return hr;
}

static HRESULT write_buffer(struct mf_save *s, DWORD stream, IMFMediaBuffer *buf, LONGLONG time, LONGLONG duration)
{
	IMFSample *sample = NULL;
	HRESULT hr = MFCreateSample(&sample);
	if (SUCCEEDED(hr))
		hr = IMFSample_AddBuffer(sample, buf);
	if (SUCCEEDED(hr))
		hr = IMFSample_SetSampleTime(sample, time);
	if (SUCCEEDED(hr))
		hr = IMFSample_SetSampleDuration(sample, duration);
	if (SUCCEEDED(hr))
		hr = IMFSinkWriter_WriteSample(s->writer, stream, sample);
	if (sample)
		IMFSample_Release(sample);
	return hr;
}

/* ------------------------------------------------------------------ */
/*  Video                                                              */
/* ------------------------------------------------------------------ */

static bool update_scaler(struct mf_save *s, const struct obs_source_frame *src)
{
	if (s->scaler && s->scaler_format == src->format && s->scaler_width == src->width &&
	    s->scaler_height == src->height && s->scaler_full_range == src->full_range)
		return true;

	if (s->scaler) {
		video_scaler_destroy(s->scaler);
		s->scaler = NULL;
	}

	struct video_scale_info ssi;
	memset(&ssi, 0, sizeof(ssi));
	ssi.format = src->format;
	ssi.width = src->width;
	ssi.height = src->height;
	ssi.range = src->full_range ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL;
	ssi.colorspace = s->cs;

	struct video_scale_info dsi;
	memset(&dsi, 0, sizeof(dsi));
	dsi.format = VIDEO_FORMAT_NV12;
	dsi.width = s->width;
	dsi.height = s->height;
	dsi.range = VIDEO_RANGE_PARTIAL;
	dsi.colorspace = s->cs;

	if (video_scaler_create(&s->scaler, &dsi, &ssi, VIDEO_SCALE_DEFAULT) != VIDEO_SCALER_SUCCESS) {
		s->scaler = NULL;
		return false;
	}
	s->scaler_format = src->format;
	s->scaler_width = src->width;
	s->scaler_height = src->height;
	s->scaler_full_range = src->full_range;
	return true;
}

static HRESULT write_video_frame(struct mf_save *s, const struct obs_source_frame *src, LONGLONG time)
{
	if (!update_scaler(s, src))
		return E_FAIL;

	const DWORD y_size = s->width * s->height;
	const DWORD size = y_size + y_size / 2;
	IMFMediaBuffer *buf = NULL;
	BYTE *ptr = NULL;

	HRESULT hr = MFCreateMemoryBuffer(size, &buf);
	if (SUCCEEDED(hr))
		hr = IMFMediaBuffer_Lock(buf, &ptr, NULL, NULL);
	if (SUCCEEDED(hr)) {
		uint8_t *out_data[MAX_AV_PLANES] = {ptr, ptr + y_size};
		uint32_t out_linesize[MAX_AV_PLANES] = {s->width, s->width};
		bool scaled = video_scaler_scale(s->scaler, out_data, out_linesize, (const uint8_t *const *)src->data,
						 src->linesize);
		IMFMediaBuffer_Unlock(buf);
		hr = scaled ? IMFMediaBuffer_SetCurrentLength(buf, size) : E_FAIL;
	}
	if (SUCCEEDED(hr))
		hr = write_buffer(s, s->video_stream, buf, time, s->frame_duration);
	if (buf)
		IMFMediaBuffer_Release(buf);
	return hr;
}

/* ------------------------------------------------------------------ */
/*  Audio                                                              */
/* ------------------------------------------------------------------ */

/* Writes count samples of a (a == NULL: silence) starting at skip, as 16-bit PCM. */
static HRESULT write_audio(struct mf_save *s, const struct obs_audio_data *a, int64_t skip, int64_t count)
{
	const DWORD size = (DWORD)(count * s->channels * (int64_t)sizeof(int16_t));
	IMFMediaBuffer *buf = NULL;
	BYTE *ptr = NULL;

	HRESULT hr = MFCreateMemoryBuffer(size, &buf);
	if (SUCCEEDED(hr))
		hr = IMFMediaBuffer_Lock(buf, &ptr, NULL, NULL);
	if (SUCCEEDED(hr)) {
		int16_t *out = (int16_t *)ptr;
		for (int ch = 0; ch < s->channels; ch++) {
			const float *in = (a && a->data[ch]) ? (const float *)a->data[ch] + skip : NULL;
			for (int64_t i = 0; i < count; i++) {
				float v = in ? in[i] : 0.0f;
				if (v > 1.0f)
					v = 1.0f;
				else if (v < -1.0f)
					v = -1.0f;
				out[i * s->channels + ch] = (int16_t)(v * 32767.0f);
			}
		}
		IMFMediaBuffer_Unlock(buf);
		hr = IMFMediaBuffer_SetCurrentLength(buf, size);
	}
	if (SUCCEEDED(hr))
		hr = write_buffer(s, s->audio_stream, buf, s->next_sample * HNS_PER_SEC / s->rate,
				  count * HNS_PER_SEC / s->rate);
	if (buf)
		IMFMediaBuffer_Release(buf);

	s->next_sample += count;
	return hr;
}

/* Writes replay audio packets starting before limit_sample, placed on the
 * video timeline: audio before the first video frame is trimmed and real
 * gaps are filled with silence. */
static HRESULT feed_audio_until(struct mf_save *s, const struct replay_save_job *job, uint64_t first_ts,
				int64_t limit_sample)
{
	/* Small timestamp jitter between packets is treated as contiguous. */
	const int64_t tolerance = s->rate / 100;

	while (s->packet_index < job->audio_frame_count && s->next_sample < s->end_sample) {
		const struct obs_audio_data *a = &job->audio_frames[s->packet_index];
		const int64_t start = ns_to_samples((int64_t)a->timestamp - (int64_t)first_ts, s->rate);
		if (start >= limit_sample)
			break;
		s->packet_index++;

		int64_t skip = 0;
		if (start > s->next_sample + tolerance) {
			int64_t gap = start - s->next_sample;
			if (s->next_sample + gap > s->end_sample)
				gap = s->end_sample - s->next_sample;
			HRESULT hr = write_audio(s, NULL, 0, gap);
			if (FAILED(hr))
				return hr;
		} else if (start < s->next_sample - tolerance) {
			skip = s->next_sample - start;
		}

		if (skip >= (int64_t)a->frames)
			continue;
		int64_t count = (int64_t)a->frames - skip;
		if (s->next_sample + count > s->end_sample)
			count = s->end_sample - s->next_sample;
		if (count > 0) {
			HRESULT hr = write_audio(s, a, skip, count);
			if (FAILED(hr))
				return hr;
		}
	}
	return S_OK;
}

/* Number of channels to save (stereo max), or 0 when the replay audio is unusable. */
static int audio_channels(const struct replay_save_job *job, const char *log_name)
{
	if (!job->audio_frames || !job->audio_frame_count)
		return 0;
	if (job->oai.samples_per_sec != 44100 && job->oai.samples_per_sec != 48000) {
		blog(LOG_WARNING, "[replay_source: '%s'] AAC needs 44.1 or 48 kHz audio (got %u Hz), saving without audio",
		     log_name, job->oai.samples_per_sec);
		return 0;
	}

	const struct obs_audio_data *first = &job->audio_frames[0];
	int channels = 0;
	while (channels < 2 && channels < (int)get_audio_channels(job->oai.speakers) && first->data[channels])
		channels++;
	return channels;
}

/* Writes all frames and audio to an opened writer and finalizes the file. */
static HRESULT write_all(struct mf_save *s, const struct replay_save_job *job, uint64_t first_ts)
{
	HRESULT hr = S_OK;

	s->last_video_time = -1;
	s->next_sample = 0;
	s->packet_index = 0;

	for (uint64_t i = 0; i < job->frame_count && SUCCEEDED(hr); i++) {
		const struct obs_source_frame *frame = job->video_frames[i];
		if (!frame)
			continue;

		int64_t rel_ns = (int64_t)(job->video_timestamps[i] - first_ts);
		if (rel_ns < 0)
			rel_ns = 0;

		/* Interleave: write the audio that plays up to this video frame first. */
		if (s->has_audio)
			hr = feed_audio_until(s, job, first_ts, ns_to_samples(rel_ns, s->rate));

		LONGLONG time = rel_ns / 100;
		if (time <= s->last_video_time)
			time = s->last_video_time + 1;
		s->last_video_time = time;

		if (SUCCEEDED(hr))
			hr = write_video_frame(s, frame, time);
	}
	if (SUCCEEDED(hr) && s->has_audio)
		hr = feed_audio_until(s, job, first_ts, INT64_MAX);
	if (SUCCEEDED(hr))
		hr = IMFSinkWriter_Finalize(s->writer);
	return hr;
}

/* ------------------------------------------------------------------ */
/*  Public entry point                                                 */
/* ------------------------------------------------------------------ */

bool replay_save_encode_mp4(const struct replay_save_job *job, const char *log_name)
{
	/* Hardware encoder first, then software; drop audio only as a last resort. */
	static const struct {
		bool hardware;
		bool audio;
	} attempts[] = {{true, true}, {false, true}, {true, false}, {false, false}};

	struct mf_save s;
	wchar_t *path = NULL;
	uint64_t first_ts = 0;
	uint64_t last_ts = 0;
	bool have_frame = false;
	bool success = false;
	HRESULT hr = E_FAIL;

	memset(&s, 0, sizeof(s));

	for (uint64_t i = 0; i < job->frame_count; i++) {
		if (!job->video_frames[i])
			continue;
		if (!have_frame)
			first_ts = job->video_timestamps[i];
		last_ts = job->video_timestamps[i];
		have_frame = true;
	}
	if (!have_frame || !job->fps_num || !job->fps_den)
		return false;

	/* NV12 / H.264 need even dimensions. */
	s.width = job->width & ~1u;
	s.height = job->height & ~1u;
	if (s.width < 2 || s.height < 2)
		return false;
	s.cs = job->colorspace == VIDEO_CS_601 ? VIDEO_CS_601 : VIDEO_CS_709;
	s.frame_duration = HNS_PER_SEC * job->fps_den / job->fps_num;
	s.last_video_time = -1;

	s.channels = audio_channels(job, log_name);
	s.rate = job->oai.samples_per_sec;
	if (s.channels)
		s.end_sample = ns_to_samples((int64_t)(last_ts - first_ts) + s.frame_duration * 100, s.rate);

	if (!os_utf8_to_wcs_ptr(job->file_path, 0, &path))
		return false;

	const HRESULT co_hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
	const HRESULT mf_hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
	if (FAILED(mf_hr))
		blog(LOG_WARNING, "[replay_source: '%s'] Media Foundation unavailable (0x%08lX)", log_name, mf_hr);

	for (size_t i = 0; SUCCEEDED(mf_hr) && !success && i < sizeof(attempts) / sizeof(attempts[0]); i++) {
		if (attempts[i].audio && !s.channels)
			continue;

		hr = open_writer(&s, job, path, attempts[i].hardware, attempts[i].audio);
		if (SUCCEEDED(hr))
			hr = write_all(&s, job, first_ts);
		close_writer(&s);

		if (SUCCEEDED(hr)) {
			success = true;
			blog(LOG_INFO, "[replay_source: '%s'] MP4 written with %s H.264 encoder%s", log_name,
			     attempts[i].hardware ? "hardware (if available)" : "software",
			     attempts[i].audio ? " + AAC audio" : ", no audio");
		} else {
			blog(LOG_WARNING, "[replay_source: '%s'] MP4 attempt failed (%s%s, 0x%08lX)", log_name,
			     attempts[i].hardware ? "hardware" : "software", attempts[i].audio ? " + audio" : "", hr);
		}
	}

	if (s.scaler)
		video_scaler_destroy(s.scaler);
	if (SUCCEEDED(mf_hr))
		MFShutdown();
	if (SUCCEEDED(co_hr))
		CoUninitialize();
	bfree(path);
	return success;
}

#else /* !_WIN32 */

bool replay_save_encode_mp4(const struct replay_save_job *job, const char *log_name)
{
	UNUSED_PARAMETER(job);
	blog(LOG_WARNING, "[replay_source: '%s'] MP4 saving is only available on Windows", log_name);
	return false;
}

#endif /* _WIN32 */
