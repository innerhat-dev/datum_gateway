/*
 * In-memory + on-disk hashrate history for DATUM Gateway dashboard charts.
 * Samples every 60s; keeps 24h; SVG rendered server-side (no JS).
 */
#ifndef _DATUM_HASHRATE_HIST_H_
#define _DATUM_HASHRATE_HIST_H_

#include <stddef.h>
#include <stdint.h>

#define DATUM_HR_SAMPLE_SEC       60
#define DATUM_HR_HISTORY_SAMPLES  (24 * 60)  /* 1440 @ 60s */
#define DATUM_HR_MAX_CLIENTS      128
#define DATUM_HR_KEY_LEN          64
#define DATUM_HR_SVG_MAX_POINTS   120

/* range_sec: 3600, 21600, or 86400 (clamped) */
int datum_hashrate_hist_parse_range(const char *s);

void datum_hashrate_hist_init(const char *path);
void datum_hashrate_hist_shutdown(void);
void datum_hashrate_hist_tick(void);

/* Fill SVG + stats into buffer. client_key NULL = total gateway hashrate */
size_t datum_hashrate_hist_render_svg(char *out, size_t out_sz,
	const char *client_key, int range_sec,
	double *out_current, double *out_avg, double *out_peak);

/* Compact sparkline SVG for table cells */
size_t datum_hashrate_hist_render_sparkline(char *out, size_t out_sz,
	const char *client_key, int range_sec);

#endif
