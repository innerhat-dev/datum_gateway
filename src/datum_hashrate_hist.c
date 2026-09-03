/*
 * DATUM Gateway — hashrate history (60s samples, 24h ring, disk persistence)
 */

#include <errno.h>
#include <stdint.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "datum_hashrate_hist.h"
#include "datum_logger.h"
#include "datum_stratum.h"
#include "datum_utils.h"

#define HR_MAGIC 0x48524831u /* HRH1 */
#define HR_VERSION 1

typedef struct {
	char key[DATUM_HR_KEY_LEN];
	uint64_t last_seen_sec;
	double th[DATUM_HR_HISTORY_SAMPLES];
	int in_use;
} hr_client_t;

static pthread_mutex_t hr_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t hr_thread;
static int hr_thread_started = 0;
static int hr_running = 0;

static char hr_path[1024];
static uint64_t sample_ts[DATUM_HR_HISTORY_SAMPLES];
static double total_th[DATUM_HR_HISTORY_SAMPLES];
static int hr_head = 0;   /* next write index */
static int hr_count = 0;
static hr_client_t clients[DATUM_HR_MAX_CLIENTS];
static uint64_t last_sample_sec = 0;
static int dirty = 0;

int datum_hashrate_hist_parse_range(const char *s) {
	if (!s || !s[0]) return 86400;
	if (!strcmp(s, "1h") || !strcmp(s, "3600")) return 3600;
	if (!strcmp(s, "6h") || !strcmp(s, "21600")) return 21600;
	if (!strcmp(s, "24h") || !strcmp(s, "86400")) return 86400;
	return 86400;
}

static double client_est_th(T_DATUM_MINER_DATA *m, uint64_t tsms) {
	unsigned char astat;
	double hr = 0.0;
	if (!m || !m->subscribed) return 0.0;
	astat = m->stats.active_index ? 0 : 1;
	if ((m->stats.last_swap_ms > 0) && (m->stats.diff_accepted[astat] > 0)) {
		hr = ((double)m->stats.diff_accepted[astat] / (double)((double)m->stats.last_swap_ms / 1000.0)) * 0.004294967296;
	}
	if (((double)(tsms - m->stats.last_swap_tsms) / 1000.0) >= 180.0) {
		return 0.0;
	}
	return hr;
}

static void make_client_key(char *key, size_t key_sz, T_DATUM_MINER_DATA *m, int tid, int cid) {
	if (m && m->last_auth_username[0]) {
		snprintf(key, key_sz, "%s", m->last_auth_username);
		return;
	}
	if (m && m->unique_id) {
		snprintf(key, key_sz, "id-%llu", (unsigned long long)m->unique_id);
		return;
	}
	snprintf(key, key_sz, "conn-%d/%d", tid, cid);
}

static int find_client_slot(const char *key, int create) {
	int i, empty = -1, oldest = -1;
	uint64_t oldest_seen = UINT64_MAX;
	for (i = 0; i < DATUM_HR_MAX_CLIENTS; i++) {
		if (clients[i].in_use) {
			if (strncmp(clients[i].key, key, DATUM_HR_KEY_LEN) == 0) return i;
			if (clients[i].last_seen_sec < oldest_seen) {
				oldest_seen = clients[i].last_seen_sec;
				oldest = i;
			}
		} else if (empty < 0) {
			empty = i;
		}
	}
	if (!create) return -1;
	i = (empty >= 0) ? empty : oldest;
	if (i < 0) return -1;
	memset(&clients[i], 0, sizeof(clients[i]));
	snprintf(clients[i].key, sizeof(clients[i].key), "%s", key);
	clients[i].in_use = 1;
	return i;
}

static void hr_save_unlocked(void) {
	FILE *f;
	uint32_t magic = HR_MAGIC, ver = HR_VERSION;
	int i;
	if (!hr_path[0]) return;
	f = fopen(hr_path, "wb");
	if (!f) {
		DLOG_ERROR("hashrate history: cannot write %s: %s", hr_path, strerror(errno));
		return;
	}
	fwrite(&magic, 4, 1, f);
	fwrite(&ver, 4, 1, f);
	fwrite(&hr_head, sizeof(hr_head), 1, f);
	fwrite(&hr_count, sizeof(hr_count), 1, f);
	fwrite(sample_ts, sizeof(sample_ts), 1, f);
	fwrite(total_th, sizeof(total_th), 1, f);
	for (i = 0; i < DATUM_HR_MAX_CLIENTS; i++) {
		fwrite(&clients[i], sizeof(clients[i]), 1, f);
	}
	fclose(f);
	dirty = 0;
}

static void hr_load_unlocked(void) {
	FILE *f;
	uint32_t magic = 0, ver = 0;
	if (!hr_path[0]) return;
	f = fopen(hr_path, "rb");
	if (!f) return;
	if (fread(&magic, 4, 1, f) != 1 || magic != HR_MAGIC) { fclose(f); return; }
	if (fread(&ver, 4, 1, f) != 1 || ver != HR_VERSION) { fclose(f); return; }
	if (fread(&hr_head, sizeof(hr_head), 1, f) != 1) { fclose(f); return; }
	if (fread(&hr_count, sizeof(hr_count), 1, f) != 1) { fclose(f); return; }
	if (hr_head < 0 || hr_head >= DATUM_HR_HISTORY_SAMPLES ||
	    hr_count < 0 || hr_count > DATUM_HR_HISTORY_SAMPLES) {
		hr_head = 0; hr_count = 0; fclose(f); return;
	}
	if (fread(sample_ts, sizeof(sample_ts), 1, f) != 1) { fclose(f); hr_count = 0; return; }
	if (fread(total_th, sizeof(total_th), 1, f) != 1) { fclose(f); hr_count = 0; return; }
	if (fread(clients, sizeof(clients), 1, f) != 1) {
		memset(clients, 0, sizeof(clients));
	}
	fclose(f);
	DLOG_INFO("hashrate history: loaded %d samples from %s", hr_count, hr_path);
}

static void hr_sample_unlocked(void) {
	uint64_t tsms = current_time_millis();
	uint64_t now_sec = tsms / 1000;
	double thr = 0.0;
	int j, ii, slot;
	char key[DATUM_HR_KEY_LEN];
	T_DATUM_MINER_DATA *m;
	double hr;
	int seen[DATUM_HR_MAX_CLIENTS];

	if (last_sample_sec && now_sec < last_sample_sec + DATUM_HR_SAMPLE_SEC) return;
	last_sample_sec = now_sec;

	memset(seen, 0, sizeof(seen));

	/* zero this slot for all clients first */
	for (j = 0; j < DATUM_HR_MAX_CLIENTS; j++) {
		if (clients[j].in_use) clients[j].th[hr_head] = 0.0;
	}

	if (global_stratum_app) {
		for (j = 0; j < global_stratum_app->max_threads; j++) {
			for (ii = 0; ii < global_stratum_app->max_clients_thread; ii++) {
				if (global_stratum_app->datum_threads[j].client_data[ii].fd <= 0) continue;
				m = (T_DATUM_MINER_DATA *)global_stratum_app->datum_threads[j].client_data[ii].app_client_data;
				hr = client_est_th(m, tsms);
				thr += hr;
				make_client_key(key, sizeof(key), m, j, ii);
				slot = find_client_slot(key, 1);
				if (slot >= 0) {
					clients[slot].th[hr_head] += hr;
					clients[slot].last_seen_sec = now_sec;
					seen[slot] = 1;
				}
			}
		}
	}

	sample_ts[hr_head] = now_sec;
	total_th[hr_head] = thr;
	hr_head = (hr_head + 1) % DATUM_HR_HISTORY_SAMPLES;
	if (hr_count < DATUM_HR_HISTORY_SAMPLES) hr_count++;
	dirty = 1;

	/* persist every sample (60s) — small file, safe */
	hr_save_unlocked();
}

void datum_hashrate_hist_tick(void) {
	pthread_mutex_lock(&hr_lock);
	hr_sample_unlocked();
	pthread_mutex_unlock(&hr_lock);
}

static void *hr_thread_fn(void *arg) {
	(void)arg;
	while (hr_running) {
		datum_hashrate_hist_tick();
		sleep(DATUM_HR_SAMPLE_SEC);
	}
	return NULL;
}

void datum_hashrate_hist_init(const char *path) {
	pthread_mutex_lock(&hr_lock);
	memset(sample_ts, 0, sizeof(sample_ts));
	memset(total_th, 0, sizeof(total_th));
	memset(clients, 0, sizeof(clients));
	hr_head = 0;
	hr_count = 0;
	hr_path[0] = 0;
	if (path && path[0]) {
		snprintf(hr_path, sizeof(hr_path), "%s", path);
		hr_load_unlocked();
	}
	pthread_mutex_unlock(&hr_lock);
	hr_running = 1;
	if (!hr_thread_started) {
		if (pthread_create(&hr_thread, NULL, hr_thread_fn, NULL) == 0) {
			hr_thread_started = 1;
			DLOG_INFO("hashrate history: sampling every %ds, file=%s", DATUM_HR_SAMPLE_SEC, hr_path[0] ? hr_path : "(none)");
		} else {
			DLOG_ERROR("hashrate history: failed to start sampler thread");
		}
	}
}

void datum_hashrate_hist_shutdown(void) {
	hr_running = 0;
	if (hr_thread_started) {
		pthread_join(hr_thread, NULL);
		hr_thread_started = 0;
	}
	pthread_mutex_lock(&hr_lock);
	if (dirty) hr_save_unlocked();
	pthread_mutex_unlock(&hr_lock);
}

static int collect_window(const double *series, int range_sec,
	double *out_v, uint64_t *out_t, int max_out,
	double *cur, double *avg, double *peak) {
	int n = 0, i, idx;
	uint64_t now_sec, cutoff;
	double sum = 0, pk = 0;

	if (hr_count <= 0) {
		if (cur) *cur = 0;
		if (avg) *avg = 0;
		if (peak) *peak = 0;
		return 0;
	}

	now_sec = sample_ts[(hr_head + DATUM_HR_HISTORY_SAMPLES - 1) % DATUM_HR_HISTORY_SAMPLES];
	if (!now_sec) now_sec = (uint64_t)time(NULL);
	cutoff = (range_sec > 0 && (uint64_t)range_sec < now_sec) ? now_sec - (uint64_t)range_sec : 0;

	for (i = 0; i < hr_count; i++) {
		idx = (hr_head + DATUM_HR_HISTORY_SAMPLES - hr_count + i) % DATUM_HR_HISTORY_SAMPLES;
		if (sample_ts[idx] < cutoff) continue;
		if (n < max_out) {
			out_v[n] = series ? series[idx] : total_th[idx];
			out_t[n] = sample_ts[idx];
			n++;
		}
	}

	if (n == 0) {
		if (cur) *cur = 0;
		if (avg) *avg = 0;
		if (peak) *peak = 0;
		return 0;
	}

	/* downsample if needed */
	if (n > DATUM_HR_SVG_MAX_POINTS) {
		int target = DATUM_HR_SVG_MAX_POINTS;
		double nv[DATUM_HR_SVG_MAX_POINTS];
		uint64_t nt[DATUM_HR_SVG_MAX_POINTS];
		for (i = 0; i < target; i++) {
			int src = (int)((long long)i * (n - 1) / (target - 1));
			nv[i] = out_v[src];
			nt[i] = out_t[src];
		}
		memcpy(out_v, nv, sizeof(double) * target);
		memcpy(out_t, nt, sizeof(uint64_t) * target);
		n = target;
	}

	for (i = 0; i < n; i++) {
		sum += out_v[i];
		if (out_v[i] > pk) pk = out_v[i];
	}
	if (cur) *cur = out_v[n - 1];
	if (avg) *avg = sum / (double)n;
	if (peak) *peak = pk;
	return n;
}

static size_t render_svg_inner(char *out, size_t out_sz,
	const double *vals, const uint64_t *times, int n,
	int width, int height, int spark) {
	int i;
	double vmax = 0.0, vmin = 0.0;
	int pad_l = spark ? 2 : 48;
	int pad_r = spark ? 2 : 12;
	int pad_t = spark ? 2 : 16;
	int pad_b = spark ? 2 : 28;
	int plot_w = width - pad_l - pad_r;
	int plot_h = height - pad_t - pad_b;
	size_t sz = 0;
	char path[8192];
	size_t psz = 0;

	if (n < 1 || plot_w < 10 || plot_h < 10) {
		return snprintf(out, out_sz, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%d\" height=\"%d\"></svg>", width, height);
	}

	for (i = 0; i < n; i++) {
		if (vals[i] > vmax) vmax = vals[i];
	}
	if (vmax <= 0.0) vmax = 1.0;
	vmax *= 1.05;

	path[0] = 0;
	for (i = 0; i < n; i++) {
		double x = pad_l + (n == 1 ? plot_w / 2.0 : (double)i * plot_w / (double)(n - 1));
		double y = pad_t + plot_h - (vals[i] / vmax) * plot_h;
		psz += snprintf(path + psz, sizeof(path) - psz, "%s%.1f,%.1f", i ? " L" : "M", x, y);
		if (psz >= sizeof(path) - 32) break;
	}

	sz += snprintf(out + sz, out_sz - sz,
		"<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 %d %d\" width=\"100%%\" height=\"%d\" class=\"hr-chart%s\">",
		width, height, height, spark ? " hr-spark" : "");

	if (!spark) {
		/* grid */
		for (i = 0; i <= 4; i++) {
			double y = pad_t + plot_h * i / 4.0;
			double gv = vmax * (4 - i) / 4.0;
			sz += snprintf(out + sz, out_sz - sz,
				"<line x1=\"%d\" y1=\"%.1f\" x2=\"%d\" y2=\"%.1f\" class=\"hr-grid\"/>",
				pad_l, y, width - pad_r, y);
			sz += snprintf(out + sz, out_sz - sz,
				"<text x=\"%d\" y=\"%.1f\" class=\"hr-axis\">%.2f</text>",
				4, y + 4, gv);
		}
		/* time labels ends */
		{
			time_t t0 = (time_t)times[0], t1 = (time_t)times[n - 1];
			struct tm tm0, tm1;
			char b0[32], b1[32];
			gmtime_r(&t0, &tm0);
			gmtime_r(&t1, &tm1);
			strftime(b0, sizeof(b0), "%H:%MZ", &tm0);
			strftime(b1, sizeof(b1), "%H:%MZ", &tm1);
			sz += snprintf(out + sz, out_sz - sz,
				"<text x=\"%d\" y=\"%d\" class=\"hr-axis\">%s</text>", pad_l, height - 6, b0);
			sz += snprintf(out + sz, out_sz - sz,
				"<text x=\"%d\" y=\"%d\" text-anchor=\"end\" class=\"hr-axis\">%s</text>",
				width - pad_r, height - 6, b1);
		}
	}

	sz += snprintf(out + sz, out_sz - sz,
		"<path d=\"%s\" fill=\"none\" class=\"hr-line\"/>", path);
	sz += snprintf(out + sz, out_sz - sz, "</svg>");
	return sz;
}

size_t datum_hashrate_hist_render_svg(char *out, size_t out_sz,
	const char *client_key, int range_sec,
	double *out_current, double *out_avg, double *out_peak) {
	double vals[DATUM_HR_HISTORY_SAMPLES];
	uint64_t times[DATUM_HR_HISTORY_SAMPLES];
	const double *series = NULL;
	int n, slot = -1;
	size_t sz = 0;
	double cur = 0, avg = 0, peak = 0;

	if (!out || out_sz < 64) return 0;
	if (range_sec <= 0) range_sec = 86400;

	pthread_mutex_lock(&hr_lock);
	if (client_key && client_key[0]) {
		slot = find_client_slot(client_key, 0);
		if (slot >= 0) series = clients[slot].th;
	}
	n = collect_window(series, range_sec, vals, times, DATUM_HR_HISTORY_SAMPLES, &cur, &avg, &peak);
	sz = render_svg_inner(out, out_sz, vals, times, n, 720, 200, 0);
	pthread_mutex_unlock(&hr_lock);

	if (out_current) *out_current = cur;
	if (out_avg) *out_avg = avg;
	if (out_peak) *out_peak = peak;
	return sz;
}

size_t datum_hashrate_hist_render_sparkline(char *out, size_t out_sz,
	const char *client_key, int range_sec) {
	double vals[DATUM_HR_HISTORY_SAMPLES];
	uint64_t times[DATUM_HR_HISTORY_SAMPLES];
	const double *series = NULL;
	int n, slot;
	size_t sz;

	if (!out || out_sz < 32) return 0;
	if (range_sec <= 0) range_sec = 86400;

	pthread_mutex_lock(&hr_lock);
	if (client_key && client_key[0]) {
		slot = find_client_slot(client_key, 0);
		if (slot >= 0) series = clients[slot].th;
		else { pthread_mutex_unlock(&hr_lock); return snprintf(out, out_sz, "—"); }
	}
	n = collect_window(series, range_sec, vals, times, DATUM_HR_HISTORY_SAMPLES, NULL, NULL, NULL);
	sz = render_svg_inner(out, out_sz, vals, times, n, 120, 28, 1);
	pthread_mutex_unlock(&hr_lock);
	return sz;
}
