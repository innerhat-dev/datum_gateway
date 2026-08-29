/*
 *
 * DATUM Gateway
 * Decentralized Alternative Templates for Universal Mining
 *
 * This file is part of the DATUM Gateway and is distributed under the
 * same MIT license as the rest of the project. See LICENSE.
 *
 */

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "datum_stratum.h"
#include "datum_conf.h"
#include "datum_coinbaser.h"
#include "datum_utils.h"

static void datum_coinbaser_tests_layout(void) {
	unsigned char buf[256];
	size_t n;
	
	n = datum_coinbaser_tag_bytes("A", "B", true, buf, sizeof(buf));
	datum_test(n == 3);
	datum_test(memcmp(buf, "A\x0F" "B", 3) == 0);
	
	n = datum_coinbaser_tag_bytes("A", "", true, buf, sizeof(buf));
	datum_test(n == 1 && buf[0] == 'A');
	
	n = datum_coinbaser_tag_bytes("", "B", true, buf, sizeof(buf));
	datum_test(n == 2 && buf[0] == 0x0F && buf[1] == 'B');
	
	n = datum_coinbaser_tag_bytes("", "", true, buf, sizeof(buf));
	datum_test(n == 0);
}

static void datum_coinbaser_tests_trim(void) {
	// 60-byte primary, the most the config allows
	const char *p60 = "123456789012345678901234567890123456789012345678901234567890";
	unsigned char full[256], cut[256];
	size_t n_full, n_cut;
	
	// 60 + 24 = 84: fits, nothing is cut
	n_full = datum_coinbaser_tag_bytes(p60, "123456789012345678901234", false, full, sizeof(full));
	n_cut = datum_coinbaser_tag_bytes(p60, "123456789012345678901234", true, cut, sizeof(cut));
	datum_test(n_full == 85 && n_cut == n_full);
	datum_test(memcmp(full, cut, n_cut) == 0);
	
	// 60 + 25 = 85: the config check allows up to 88, the coinbaser cuts one byte off the secondary
	n_full = datum_coinbaser_tag_bytes(p60, "1234567890123456789012345", false, full, sizeof(full));
	n_cut = datum_coinbaser_tag_bytes(p60, "1234567890123456789012345", true, cut, sizeof(cut));
	datum_test(n_full == 86 && n_cut == 85);
	datum_test(memcmp(full, cut, n_cut) == 0 && cut[n_cut - 1] == '4');
	
	// 60 + 28 = 88: still allowed by the config check, four bytes cut
	n_full = datum_coinbaser_tag_bytes(p60, "1234567890123456789012345678", false, full, sizeof(full));
	n_cut = datum_coinbaser_tag_bytes(p60, "1234567890123456789012345678", true, cut, sizeof(cut));
	datum_test(n_full == 89 && n_cut == 85);
	datum_test(memcmp(full, cut, n_cut) == 0 && cut[n_cut - 1] == '4');
	
	// Only the secondary is ever trimmed. A pool-override primary can exceed
	// the 60-byte config cap; when the excess reaches the whole secondary,
	// the secondary is dropped and the primary rides alone.
	n_full = datum_coinbaser_tag_bytes("123456789012345678901234567890123456789012345678901234567890123456789012345678901234", "12", false, full, sizeof(full));
	n_cut = datum_coinbaser_tag_bytes("123456789012345678901234567890123456789012345678901234567890123456789012345678901234", "12", true, cut, sizeof(cut));
	datum_test(n_full == 87 && n_cut == 84);
	datum_test(memcmp(full, cut, 84) == 0 && cut[83] == '4');
}

static void datum_coinbaser_tests_input(void) {
	// generate_coinbase_input emits the helper's bytes; hold it to that:
	// push length is the bytes written plus the terminator, payload matches.
	const char *p60 = "123456789012345678901234567890123456789012345678901234567890";
	const struct { const char *primary; const char *secondary; } cases[] = {
		{ "DATUM Gateway", "Catbus" },              // plain, one-byte push
		{ p60, "1234567890123456789012345678" },    // trimmed, two-byte push
		{ "solo", "" },                             // primary only
	};
	unsigned char bin[300], expect[256];
	char cb[600];
	size_t n, i, c;
	int cb_len, pot, off, push_len;
	
	for (c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
		strcpy(datum_config.mining_coinbase_tag_primary, cases[c].primary);
		strcpy(datum_config.mining_coinbase_tag_secondary, cases[c].secondary);
		cb_len = generate_coinbase_input(1000, cb, &pot);
		datum_test(cb_len > 0 && cb_len < 150);
		for (i = 0; i < (size_t)cb_len; i++) {
			bin[i] = hex2bin_uchar(&cb[i << 1]);
		}
		off = 1 + bin[0]; // BIP34 height push
		if (bin[off] == 0x4c) {
			push_len = bin[off + 1]; off += 2;
		} else {
			push_len = bin[off]; off += 1;
		}
		n = datum_coinbaser_tag_bytes(cases[c].primary, cases[c].secondary, true, expect, sizeof(expect));
		datum_test(push_len == (int)n + 1);
		datum_test(memcmp(&bin[off], expect, n) == 0);
		datum_test(bin[off + n] == 0x00);
	}
}

void datum_coinbaser_tests(void) {
	datum_coinbaser_tests_layout();
	datum_coinbaser_tests_trim();
	datum_coinbaser_tests_input();
}
