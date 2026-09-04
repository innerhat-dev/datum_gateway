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
 * Copyright (c) 2024 Bitcoin Ocean, LLC & Jason Hughes
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

#ifndef _DATUM_COINBASE_H_
#define _DATUM_COINBASE_H_

#include <stdbool.h>
#include <stddef.h>

// Bytes available to the two coinbase tags together, separator included;
// generate_coinbase_input() cuts the secondary tag to fit.
#define MAX_COINBASE_TAG_SPACE 86

typedef enum {
	DATUM_HEADLINE_PRESENT = 0,	// found in the tag bytes the coinbaser will emit
	DATUM_HEADLINE_TRIMMED,		// found in the configured tags, lost to the tag-space cut
	DATUM_HEADLINE_MISSING,		// not in the configured tags at all
} datum_headline_status;

size_t datum_coinbaser_tag_bytes(const char *primary, const char *secondary, bool apply_trim, unsigned char *out, size_t out_sz);
datum_headline_status datum_coinbaser_headline_status(const char *primary, const char *secondary, const unsigned char *headline, size_t headline_len);
void datum_coinbaser_tests(void);

int datum_coinbaser_init(void);
void generate_coinbase_txns_for_stratum_job_subtypebysize(T_DATUM_STRATUM_JOB *s, int coinbase_index, int remaining_size, bool space_for_en_in_coinbase, int *cb1idx, int *cb2idx, bool special_coinb1);
void generate_coinbase_txns_for_stratum_job(T_DATUM_STRATUM_JOB *s, bool empty_only);
void generate_base_coinbase_txns_for_stratum_job(T_DATUM_STRATUM_JOB *s, bool new_block);
int datum_coinbaser_v2_parse(T_DATUM_STRATUM_JOB *s, unsigned char *coinbaser, int cblen, bool must_free);

#endif
