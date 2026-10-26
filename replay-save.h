#pragma once

#include <stdbool.h>

struct replay_save_job;

/* Encode a queued replay (video + audio) to the MP4 file at job->file_path.
 * Runs as fast as the encoder allows; call only from the save worker thread. */
bool replay_save_encode_mp4(const struct replay_save_job *job, const char *log_name);
