/*
 * logUnroll — concatenate DATUM named logs in block-height order.
 * GIT_COMMIT_HASH is baked in at compile time (same header as datum_gateway).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/stat.h>

#include "git_version.h"

#ifndef GIT_COMMIT_HASH
#define GIT_COMMIT_HASH "UNKNOWN_GIT_HASH"
#endif

#ifdef _WIN32
#define LOGUNROLL_NAME "logUnroll.exe"
#else
#define LOGUNROLL_NAME "logUnroll"
#endif

typedef struct {
	char *path;
	uint64_t height;
	char extra[64];
} log_ent;

static int cmp_ent(const void *a, const void *b)
{
	const log_ent *x = a, *y = b;
	if (x->height < y->height) return -1;
	if (x->height > y->height) return 1;
	return strcmp(x->extra, y->extra);
}

static int parse_datum_log_name(const char *name, uint64_t *height, char *extra, size_t extrasz)
{
	unsigned long h = 0;
	char commit[32];
	char date[32];
	int n;
	if (!name) return 0;
	n = sscanf(name, "datum_log_%lu_%31[^._]", &h, commit);
	if (n < 2) return 0;
	*height = (uint64_t)h;
	date[0] = 0;
	{
		const char *p = strstr(name, commit);
		if (p) {
			p += strlen(commit);
			if (p[0] == '_' && p[1]) {
				snprintf(date, sizeof(date), "%s", p + 1);
				char *dot = strrchr(date, '.');
				if (dot) *dot = 0;
			}
		}
	}
	snprintf(extra, extrasz, "%s\t%s", date[0] ? date : "", commit);
	return 1;
}

static int cat_file(const char *path)
{
	FILE *f;
	char buf[8192];
	size_t n;
	f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "logUnroll: cannot open %s\n", path);
		return -1;
	}
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
		if (fwrite(buf, 1, n, stdout) != n) {
			fclose(f);
			return -1;
		}
	}
	fclose(f);
	return 0;
}

static void print_usage(const char *argv0)
{
	fprintf(stderr,
		"logUnroll %s\n"
		"Usage: %s [folder]\n"
		"  Concatenate datum_log_<height>_<commit>[_<date>].log files\n"
		"  in that folder, sorted by height then date.\n"
		"  Default folder is the directory containing this binary.\n"
		"  --commit   print baked-in git commit and exit\n",
		GIT_COMMIT_HASH, argv0 ? argv0 : LOGUNROLL_NAME);
}

int main(int argc, char **argv)
{
	const char *folder = NULL;
	char selfdir[1024];
	DIR *d;
	struct dirent *ent;
	log_ent *list = NULL;
	size_t n = 0, cap = 0;
	size_t i;
	int rc = 0;

	if (argc >= 2 && (!strcmp(argv[1], "--commit") || !strcmp(argv[1], "--version") || !strcmp(argv[1], "-V"))) {
		printf("%s\n", GIT_COMMIT_HASH);
		return 0;
	}
	if (argc >= 2 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
		print_usage(argv[0]);
		return 0;
	}
	if (argc >= 2) folder = argv[1];

	if (!folder) {
		const char *slash = strrchr(argv[0], '/');
#ifdef _WIN32
		const char *b = strrchr(argv[0], '\\');
		if (!slash || (b && b > slash)) slash = b;
#endif
		if (!slash) {
			folder = ".";
		} else {
			size_t len = (size_t)(slash - argv[0]);
			if (len >= sizeof(selfdir)) len = sizeof(selfdir) - 1;
			memcpy(selfdir, argv[0], len);
			selfdir[len] = 0;
			folder = selfdir[0] ? selfdir : ".";
		}
	}

	d = opendir(folder);
	if (!d) {
		fprintf(stderr, "logUnroll: cannot open folder %s\n", folder);
		return 1;
	}
	while ((ent = readdir(d)) != NULL) {
		uint64_t height = 0;
		char extra[64];
		char path[1600];
		struct stat st;
		if (!parse_datum_log_name(ent->d_name, &height, extra, sizeof(extra))) continue;
		snprintf(path, sizeof(path), "%s/%s", folder, ent->d_name);
		if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
		if (n == cap) {
			cap = cap ? cap * 2 : 32;
			list = realloc(list, cap * sizeof(*list));
			if (!list) {
				closedir(d);
				return 1;
			}
		}
		list[n].path = strdup(path);
		list[n].height = height;
		snprintf(list[n].extra, sizeof(list[n].extra), "%s", extra);
		n++;
	}
	closedir(d);

	qsort(list, n, sizeof(*list), cmp_ent);
	for (i = 0; i < n; i++) {
		if (cat_file(list[i].path) != 0) rc = 1;
		free(list[i].path);
	}
	free(list);
	return rc;
}
