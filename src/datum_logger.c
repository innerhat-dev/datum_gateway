/*
 *
 * DATUM Gateway
 * Decentralized Alternative Templates for Universal Mining
 *
 * This file is part of OCEAN's Bitcoin mining decentralization
 * project, DATUM.
 *
 * https://ocean.xyz
 *
 * ---
 *
 * Copyright (c) 2024-2025 Bitcoin Ocean, LLC & Jason Hughes
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

// Multithread-safe logger
// TODO: Add additional output types such as for system logging.

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <ctype.h>
#include <signal.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/time.h>
#include <inttypes.h>
#include <stdbool.h>
#include <pthread.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

#include "datum_conf.h"
#include "datum_logger.h"
#include "datum_utils.h"
#include "datum_gateway.h"

const char *level_text[] = { "  ALL", "DEBUG", " INFO", " WARN", "ERROR", "FATAL" };

volatile bool datum_logger_initialized = false;
volatile bool log_reopen_signal = false;

// configurable options
bool log_to_file = false;
bool log_to_console = true;
int log_level_console = DLOG_LEVEL_INFO;
int log_level_file = DLOG_LEVEL_ALL;
bool log_calling_function = true;
bool log_to_stderr = false;
bool log_rotate_daily = true;
char log_file[1024] = { 0 };

static char log_file_configured[1024] = { 0 };

static int console_collapse_kind = 0;
static int console_collapse_count = 0;
static int console_collapse_open = 0;

static int datum_console_job_kind(const char *msg)
{
	if (!msg) return 0;
	if (!strncmp(msg, "Updating standard stratum job for block", 39)) return 1;
	if (!strncmp(msg, "Updating priority stratum job for block", 39)) return 2;
	if (!strcmp(msg, "NEW NETWORK BLOCK NOTIFICATION RECEIVED")) return 3;
	if (!strncmp(msg, "NEW NETWORK BLOCK:", 18)) return 4;
	return 0;
}

static int datum_console_is_tty(FILE *out)
{
	int fd;
	if (!out) return 0;
	fd = fileno(out);
	if (fd < 0) return 0;
	return isatty(fd);
}

static int datum_console_cols(FILE *out)
{
	struct winsize ws;
	if (ioctl(fileno(out), TIOCGWINSZ, &ws) != 0) return 0;
	return (int)ws.ws_col;
}

static void datum_console_write(FILE *out, const char *line_with_nl, const char *msg)
{
	int kind = datum_console_job_kind(msg);
	int tty = datum_console_is_tty(out);
	int cols = tty ? datum_console_cols(out) : 0;
	size_t linelen = line_with_nl ? strlen(line_with_nl) : 0;
	char rebuilt[1200];
	int can_collapse = datum_config.clog_console_collapse_jobs && tty && kind && cols > 0 && (int)linelen + 16 < cols;

	if (!can_collapse) {
		if (console_collapse_open) {
			fputc('\n', out);
			console_collapse_open = 0;
		}
		console_collapse_kind = 0;
		console_collapse_count = 0;
		fputs(line_with_nl, out);
		return;
	}

	/* Notification lines stack; the following NEW NETWORK BLOCK line eats them. */
	if (kind == 4 && console_collapse_kind == 3 && console_collapse_count > 0) {
		char base[1200];
		size_t n;
		snprintf(base, sizeof(base), "%s", line_with_nl);
		n = strlen(base);
		if (n && base[n-1] == '\n') base[--n] = 0;
		{
			const char *colon = strstr(base, ": ");
			if (colon) {
				size_t pre = (size_t)(colon + 2 - base);
				snprintf(rebuilt, sizeof(rebuilt), "%.*sx%d NOTIFICATION + %s", (int)pre, base, console_collapse_count, colon + 2);
			} else {
				snprintf(rebuilt, sizeof(rebuilt), "x%d NOTIFICATION + %s", console_collapse_count, base);
			}
		}
		if (!console_collapse_open) {
			fputs("\033[1A\r", out);
		} else {
			fputc('\r', out);
		}
		fputs(rebuilt, out);
		fputs("\033[K", out);
		fflush(out);
		console_collapse_kind = 4;
		console_collapse_count = 1;
		console_collapse_open = 1;
		return;
	}

	if (console_collapse_kind == kind && console_collapse_count > 0 && kind != 4) {
		const char *colon;
		console_collapse_count++;
		colon = strstr(line_with_nl, ": ");
		if (colon) {
			size_t pre = (size_t)(colon + 2 - line_with_nl);
			snprintf(rebuilt, sizeof(rebuilt), "%.*sx%d %s", (int)pre, line_with_nl, console_collapse_count, colon + 2);
		} else {
			snprintf(rebuilt, sizeof(rebuilt), "%s", line_with_nl);
		}
		{
			size_t n = strlen(rebuilt);
			if (n && rebuilt[n-1] == '\n') rebuilt[n-1] = 0;
		}
		if (!console_collapse_open) {
			fputs("\033[1A\r", out);
		} else {
			fputc('\r', out);
		}
		fputs(rebuilt, out);
		fputs("\033[K", out);
		fflush(out);
		console_collapse_open = 1;
		return;
	}

	if (console_collapse_open) {
		fputc('\n', out);
		console_collapse_open = 0;
	}
	console_collapse_kind = kind;
	console_collapse_count = 1;
	fputs(line_with_nl, out);
}

static const char *datum_logger_commit_last4(void)
{
	static char last4[5];
	const char *h = GIT_COMMIT_HASH;
	size_t n;
	if (!h || !h[0]) return "unkn";
	n = strlen(h);
	if (n >= 4) h += n - 4;
	snprintf(last4, sizeof(last4), "%s", h);
	return last4;
}

static void datum_logger_dir_of(const char *path, char *dir, size_t dirsz)
{
	const char *slash;
	if (!path || !dir || !dirsz) return;
	dir[0] = 0;
	slash = strrchr(path, '/');
	if (!slash) { snprintf(dir, dirsz, "./"); return; }
	{
		size_t dlen = (size_t)(slash - path + 1);
		if (dlen >= dirsz) dlen = dirsz - 1;
		memcpy(dir, path, dlen);
		dir[dlen] = 0;
	}
}

static int datum_logger_copy_file(const char *src, const char *dst)
{
	FILE *in, *out;
	char buf[8192];
	size_t n;
	if (!src || !dst || !src[0] || !dst[0]) return -1;
	in = fopen(src, "rb");
	if (!in) return -1;
	out = fopen(dst, "wb");
	if (!out) { fclose(in); return -1; }
	while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
		if (fwrite(buf, 1, n, out) != n) { fclose(in); fclose(out); return -1; }
	}
	fclose(in);
	fclose(out);
	return 0;
}

void datum_logger_note_height(uint64_t height)
{
	(void)height;
}

#ifdef _WIN32
#define DATUM_LOGUNROLL_BIN "logUnroll.exe"
#define datum_popen(c,m) _popen(c,m)
#define datum_pclose(f) _pclose(f)
#else
#define DATUM_LOGUNROLL_BIN "logUnroll"
#define datum_popen(c,m) popen(c,m)
#define datum_pclose(f) pclose(f)
#endif

static int datum_logger_exe_dir(const char *argv0, char *out, size_t outsz)
{
	char tmp[1024];
	const char *slash;
	tmp[0] = 0;
#ifndef _WIN32
	{
		ssize_t n = readlink("/proc/self/exe", tmp, sizeof(tmp) - 1);
		if (n > 0) tmp[n] = 0;
	}
#endif
	if (!tmp[0] && argv0) snprintf(tmp, sizeof(tmp), "%s", argv0);
	if (!tmp[0]) return -1;
	slash = strrchr(tmp, '/');
	if (!slash) { snprintf(out, outsz, "."); return 0; }
	{
		size_t n = (size_t)(slash - tmp);
		if (n >= outsz) n = outsz - 1;
		memcpy(out, tmp, n);
		out[n] = 0;
	}
	return 0;
}

static int datum_logger_bin_commit(const char *path, char *out, size_t outsz)
{
	char cmd[1200];
	FILE *f;
	size_t n;
	if (!path || !out || !outsz) return -1;
	out[0] = 0;
	snprintf(cmd, sizeof(cmd), "\"%s\" --commit", path);
	f = datum_popen(cmd, "r");
	if (!f) return -1;
	if (!fgets(out, (int)outsz, f)) { datum_pclose(f); return -1; }
	datum_pclose(f);
	n = strlen(out);
	while (n && (out[n-1] == '\n' || out[n-1] == '\r')) out[--n] = 0;
	return out[0] ? 0 : -1;
}

void datum_logger_install_logunroll(const char *argv0)
{
	char logdir[1024], src[1200], dst[1200], have[160];
	const char *cfg = log_file_configured[0] ? log_file_configured : log_file;
	if (!cfg || !cfg[0]) return;
	datum_logger_dir_of(cfg, logdir, sizeof(logdir));
	if (!logdir[0]) return;
	if (datum_logger_exe_dir(argv0, src, sizeof(src)) != 0) return;
	{
		size_t n = strlen(src);
		snprintf(src + n, sizeof(src) - n, "/%s", DATUM_LOGUNROLL_BIN);
	}
	snprintf(dst, sizeof(dst), "%s%s", logdir, DATUM_LOGUNROLL_BIN);
	{
		FILE *probe = fopen(src, "rb");
		if (!probe) return;
		fclose(probe);
	}
	if (datum_logger_bin_commit(dst, have, sizeof(have)) == 0 && strcmp(have, GIT_COMMIT_HASH) == 0) return;
	if (datum_logger_copy_file(src, dst) != 0) {
		DLOG_WARN("Could not install %s -> %s: %s", src, dst, strerror(errno));
		return;
	}
#ifndef _WIN32
	chmod(dst, 0755);
#endif
	DLOG_INFO("Installed %s to log folder (git %s)", DATUM_LOGUNROLL_BIN, GIT_COMMIT_HASH);
}


int dlog_queue_max_entries = 0;
int msg_buf_maxsz = DLOG_MSG_BUF_SIZE;

pthread_rwlock_t dlog_active_buffer_rwlock = PTHREAD_RWLOCK_INITIALIZER;
int dlog_active_buffer = 0;
uint64_t dlog_active_buffer_version = 0;

pthread_rwlock_t dlog_buffer_rwlock[2] = { PTHREAD_RWLOCK_INITIALIZER, PTHREAD_RWLOCK_INITIALIZER };
DLOG_MSG *dlog_queue[2];
int dlog_queue_next[2] = { 0, 0 };
uint64_t dlog_queue_version[2] = { 0, 10 };

int msg_buf_idx[2] = { 0, 0 };
char *msg_buffer[2] = { NULL, NULL };

void datum_logger_config(
	bool clog_to_file,
	bool clog_to_console,
	int clog_level_console,
	int clog_level_file,
	bool clog_calling_function,
	bool clog_to_stderr,
	bool clog_rotate_daily,
	char *clog_file
) {
	log_to_file = clog_to_file;
	log_to_console = clog_to_console;
	log_level_console = clog_level_console;
	log_level_file = clog_level_file;
	log_calling_function = clog_calling_function;
	log_to_stderr = clog_to_stderr;
	log_rotate_daily = clog_rotate_daily;
	strncpy(log_file, clog_file, 1023);
	log_file[1023] = 0;
	strncpy(log_file_configured, clog_file ? clog_file : "", 1023);
	log_file_configured[1023] = 0;
	
	if (log_level_console < 0) log_level_console = 0;
	if (log_level_console > DLOG_LEVEL_FATAL) log_level_console = DLOG_LEVEL_FATAL;
	
	if (log_level_file < 0) log_level_file = 0;
	if (log_level_file > DLOG_LEVEL_FATAL) log_level_file = DLOG_LEVEL_FATAL;
}

int datum_logger_queue_msg(const char *func, int level, const char *format, ...) {
	int buffer_id, i;
	uint64_t buffer_version;
	DLOG_MSG *msg = NULL;
	uint64_t tsms;
	va_list args;
	struct timeval tv;
	struct tm tm_info;
	char time_buffer[20];
	
	if ((level < log_level_console) && (level < log_level_file)) {
		return 0;
	}
	
	// get timestamp before messing with locks and such
	gettimeofday(&tv, NULL);
	tsms = (tv.tv_sec * 1000LL) + (tv.tv_usec / 1000LL);
	
	if (level > 5) level = 5;
	if (level < 0) level = 0;
	
	if (__builtin_expect(!datum_logger_initialized,0)) {
		// not initialized yet, so we're just going to print this to console with default settings
		if ((log_to_console) && (level >= log_level_console)) {
			va_start(args, format);
			localtime_r(&tv.tv_sec, &tm_info);
			strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S", &tm_info);
			
			if (log_calling_function) {
				fprintf(log_to_stderr?stderr:stdout, "%s.%03ld [%44s] %s: ", time_buffer, (long)(tv.tv_usec / 1000), func, level_text[level]);
			} else {
				fprintf(log_to_stderr?stderr:stdout, "%s.%03ld %s: ", time_buffer, (long)(tv.tv_usec / 1000), level_text[level]);
			}
			
			vfprintf(log_to_stderr?stderr:stdout, format, args);
			fprintf(log_to_stderr?stderr:stdout, "\n");
			
			va_end(args);
		}
		return 0;
	}
	
	// Add the msg to the logger queue
	// this is probably overkill...
	for (i=0;i<10000000;i++) {
		if (i < 99999990) { // ensure we don't get the lock on the last try and forget to unlock and crash
			// get the active buffer ID
			pthread_rwlock_rdlock(&dlog_active_buffer_rwlock);
			buffer_id = dlog_active_buffer;
			buffer_version = dlog_active_buffer_version;
			pthread_rwlock_unlock(&dlog_active_buffer_rwlock);
			
			// get a write lock for that buffer
			pthread_rwlock_wrlock(&dlog_buffer_rwlock[buffer_id]);
			
			// check for race condition on buffer swap
			if (buffer_version != dlog_queue_version[buffer_id]) {
				// Race condition!
				pthread_rwlock_unlock(&dlog_buffer_rwlock[buffer_id]);
			} else {
				// no race condition, we're good
				break;
			}
		}
	}
	
	if (i >= 10000000) {
		// we have no locks, but we also couldn't sync up on the rare race condition after 10000000 attempts
		// means something very bad is probably happening
		panic_from_thread(__LINE__);
		return -1;
	}
	
	if (dlog_queue_next[buffer_id] >= dlog_queue_max_entries) {
		// TODO: Delay quickly, hope the writer thread catches up before panicking
		pthread_rwlock_unlock(&dlog_buffer_rwlock[buffer_id]);
		panic_from_thread(__LINE__);
		return -1;
	}
	
	// store the log data!
	msg = &dlog_queue[buffer_id][dlog_queue_next[buffer_id]];
	
	// Construct and store the msg....
	memset(msg, 0, sizeof(DLOG_MSG));
	msg->level = level;
	msg->tsms = tsms;
	strncpy(msg->calling_function, func, 47);
	msg->calling_function[47] = 0;
	msg->msg = &msg_buffer[buffer_id][msg_buf_idx[buffer_id]];
	va_start(args, format);
	i = vsnprintf(msg->msg, 1023, format, args);

	// clamp i to actual written value in order not to waste buffer space
	if (i >= 1023) {
		i = 1022;
	}

	va_end(args);
	
	if (((msg_buf_idx[buffer_id]+i+2) > msg_buf_maxsz) || (dlog_queue_next[buffer_id] >= dlog_queue_max_entries)) {
		// this is ok, since we overallocate by more than 1KB on purpose
		// but not great for logging!
		
		// we won't bump things, so the next line to the logger will overwrite this one
		pthread_rwlock_unlock(&dlog_buffer_rwlock[buffer_id]);
		if ((log_to_console) && (level >= log_level_console)) {
			localtime_r(&tv.tv_sec, &tm_info);
			strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S", &tm_info);
			if (log_calling_function) {
				fprintf(log_to_stderr?stderr:stdout, "LOGGER OVERRUN:%s.%03ld [%44s] %s: %s\n", time_buffer, (long)(tv.tv_usec / 1000), func, level_text[level], msg->msg);
			} else {
				fprintf(log_to_stderr?stderr:stdout, "LOGGER OVERRUN:%s.%03ld %s: %s\n", time_buffer, (long)(tv.tv_usec / 1000), level_text[level], msg->msg);
			}
		}
		return -1;
	}
	
	msg_buf_idx[buffer_id] += i+2; // increment the index
	dlog_queue_next[buffer_id]++; // bounds check is above, since we can potentially delay to wait for the writer instead of failing here
	pthread_rwlock_unlock(&dlog_buffer_rwlock[buffer_id]);
	
	return 0;
}

time_t get_midnight_timestamp(void) {
	time_t now = time(NULL);
	struct tm tm_now;
	localtime_r(&now, &tm_now);
	tm_now.tm_hour = 0;
	tm_now.tm_min = 0;
	tm_now.tm_sec = 0;
	tm_now.tm_mday += 1;
	time_t midnight = mktime(&tm_now);
	return midnight;
}

void * datum_logger_thread(void *ptr) {
	int buffer_id,offline_buffer_id;
	int i,j;
	uint64_t sts,ets,lflush;
	time_t seconds;
	int millis;
	struct tm tm_info_storage;
	struct tm *tm_info;
	DLOG_MSG *msg;
	char time_buffer[20];
	char log_line[1200];
	FILE *log_handle = NULL;
	time_t next_log_rotate = get_midnight_timestamp();
	time_t log_file_opened = time(NULL);
	
	msg_buffer[0] = calloc((DLOG_MSG_BUF_SIZE * 2) + (1024*8),1);
	if (!msg_buffer[0]) {
		DLOG(DLOG_LEVEL_FATAL, "Could not allocate memory for logger queue!");
		panic_from_thread(__LINE__);
	}
	// split the allocation in half for the double buffering
	msg_buffer[1] = &msg_buffer[0][DLOG_MSG_BUF_SIZE + (1024*4)];
	
	dlog_queue_max_entries = (DLOG_MSG_BUF_SIZE / sizeof(DLOG_MSG)) - 1;
	if (dlog_queue_max_entries < 1024) dlog_queue_max_entries = 1024;
	dlog_queue[0] = calloc(dlog_queue_max_entries * 2 * sizeof(DLOG_MSG),1);
	if (!dlog_queue[0]) {
		DLOG(DLOG_LEVEL_FATAL, "Could not allocate memory for logger queue list!");
		panic_from_thread(__LINE__);
	}
	dlog_queue[1] = &dlog_queue[0][dlog_queue_max_entries];
	
	if ((log_to_file) && (log_file[0] != 0)) {
		log_handle = fopen(log_file,"a");
		if (!log_handle) {
			DLOG(DLOG_LEVEL_FATAL, "Could not open log file (%s): %s!", log_file, strerror(errno));
			panic_from_thread(__LINE__);
		}
	}
	
	// alert the masses.
	datum_logger_initialized = true;
	
	DLOG(DLOG_LEVEL_DEBUG, "Logging thread started! (Approximately %d MB of RAM allocated for up to %d entries per cycle)", (DLOG_MSG_BUF_SIZE * 4)/1024/1024, dlog_queue_max_entries);
	lflush = 0;
	
	while(1) {
		sts = current_time_micros();
		
		// wait for there to be msgs to log, and log them!
		// We don't need to read lock to read this, as we're the only thread that writes to it.
		buffer_id = dlog_active_buffer;
		
		pthread_rwlock_rdlock(&dlog_buffer_rwlock[buffer_id]);
		i = dlog_queue_next[buffer_id];
		pthread_rwlock_unlock(&dlog_buffer_rwlock[buffer_id]);
		
		if (i) {
			// there are msgs to write.
			// switch the writers over to the other buffer, and then work on that
			// TODO: Think through the process here, ensure no race conditions with locking all at once.
			
			// this lock prevents msgs from being queued and holds up all other threads
			// we need to release it ASAP
			pthread_rwlock_wrlock(&dlog_active_buffer_rwlock);
			
			// we'll get a lock on writing to the current buffer.
			pthread_rwlock_wrlock(&dlog_buffer_rwlock[buffer_id]);
			
			// at this point we could have threads waiting on the buffer ID, and
			// we also could have threads waiting to write to the buffer we just got a
			// write lock on if the beat the race to lock the buffer_id
			// so we must increment the version of the current buffer, which will signal it's stale
			dlog_queue_version[buffer_id]++;
			
			// no one should be waiting to write the other buffer
			offline_buffer_id = buffer_id?0:1;
			pthread_rwlock_wrlock(&dlog_buffer_rwlock[offline_buffer_id]);
			
			// we now have write locks on everything
			// increment version again, just in case
			dlog_queue_version[offline_buffer_id]++;
			
			// store the new offline buffer ID as the active
			dlog_active_buffer_version = dlog_queue_version[offline_buffer_id];
			
			// make the offline buffer the active one
			dlog_active_buffer = offline_buffer_id;
			
			// just in case
			dlog_queue_next[offline_buffer_id] = 0;
			
			// release the lock on the offline
			pthread_rwlock_unlock(&dlog_buffer_rwlock[offline_buffer_id]);
			
			// release the lock on the buffer index... which releases any threads waiting to write
			pthread_rwlock_unlock(&dlog_active_buffer_rwlock);
			
			for(i=0;i<dlog_queue_next[buffer_id];i++) {
				// do things with msgs
				msg = &dlog_queue[buffer_id][i];
				seconds = msg->tsms / 1000;
				millis = msg->tsms % 1000;
				tm_info = localtime_r(&seconds, &tm_info_storage);
				strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S", tm_info);
				if (log_calling_function) {
					j = snprintf(log_line, sizeof(log_line), "%s.%03d [%44s] %s: %s\n", time_buffer, millis, msg->calling_function, level_text[msg->level], msg->msg);
				} else {
					j = snprintf(log_line, sizeof(log_line), "%s.%03d %s: %s\n", time_buffer, millis, level_text[msg->level], msg->msg);
				}
				log_line[1199] = 0;
				
				if ((log_to_console) && (msg->level >= log_level_console)) {
					datum_console_write(log_to_stderr?stderr:stdout, log_line, msg->msg);
				}
				
				if ((log_to_file) && (msg->level >= log_level_file)) {
					if (log_handle) {
						// TODO: Error handling here.
						fwrite(log_line, j, 1, log_handle);
					}
				}
			}
			
			// all done
			msg_buf_idx[buffer_id] = 0;
			dlog_queue_next[buffer_id] = 0;
			pthread_rwlock_unlock(&dlog_buffer_rwlock[buffer_id]);
			
			fflush(stdout);
			fflush(stderr);
		}
		
		ets = current_time_micros();
		
		if ((log_to_file) && (log_handle)) {
			if ((sts - lflush) > 1000000) {
				fflush(log_handle);
				lflush = sts;
			}
		}
		
		if (log_reopen_signal && log_to_file && log_handle) {
			log_reopen_signal = false;
			DLOG(DLOG_LEVEL_DEBUG, "Reopening log file");
			
			fclose(log_handle);
			
			log_handle = fopen(log_file, "a");
			if (!log_handle) {
				DLOG(DLOG_LEVEL_FATAL, "Could not reopen log file (%s): %s!", log_file, strerror(errno));
				panic_from_thread(__LINE__);
			}
			log_file_opened = time(NULL);
		}
		
		if ((log_rotate_daily) && (log_to_file) && (log_handle)) {
			if (next_log_rotate < (ets/1000000ULL)) {
				DLOG(DLOG_LEVEL_INFO, "Rotating log file!");
				
				tm_info = localtime_r(&log_file_opened, &tm_info_storage);
				strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d", tm_info);
				snprintf(log_line, sizeof(log_line), "%s.%s", log_file, time_buffer);
				
				fclose(log_handle);

				if (rename(log_file, log_line) != 0) {
					DLOG(DLOG_LEVEL_ERROR, "Could not rename log file (%s) for rotation: %s!", log_file, strerror(errno));
				}
				
				log_handle = fopen(log_file,"a");
				if (!log_handle) {
					DLOG(DLOG_LEVEL_FATAL, "Could not open log file (%s) after rotation: %s!", log_file, strerror(errno));
					panic_from_thread(__LINE__);
				}
				
				next_log_rotate = get_midnight_timestamp()+1;
				log_file_opened = time(NULL);
			}
		}
		
		if (ets > sts) {
			ets = ets - sts;
		} else {
			ets = 0;
		}
		
		if (ets < 56999) {
			j = (57000 - ets) / 1000;
			j++;
			for(i=0;i<j;i++) {
				if (panic_mode) {
					i = j;
				} else {
					usleep(1000);
				}
			}
		}
	}
	return NULL;
}

void datum_logger_hup_signal(int signum) {
	log_reopen_signal = true;
}

int datum_logger_init(void) {
	const struct sigaction hup_sigaction = { .sa_handler = datum_logger_hup_signal, };
	if (0 != sigaction(SIGHUP, &hup_sigaction, NULL)) {
		DLOG_ERROR("Failed to setup SIGHUP handler: %s", strerror(errno));
	}
	
	pthread_t pthread_datum_logger_thread;
	
	pthread_create(&pthread_datum_logger_thread, NULL, datum_logger_thread, NULL);
	
	return 0;
}
