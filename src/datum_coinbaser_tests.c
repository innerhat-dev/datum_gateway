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

static void datum_coinbaser_tests_headline(void) {
	// 60-byte primary, the most the config allows
	const char *p60 = "123456789012345678901234567890123456789012345678901234567890";
	const unsigned char headline[] = "Catbus";
	const size_t hl = sizeof(headline) - 1;
	
	datum_test(datum_coinbaser_headline_status("DATUM Gateway", "Catbus", headline, hl) == DATUM_HEADLINE_PRESENT);
	datum_test(datum_coinbaser_headline_status("sopko", "Totoro", headline, hl) == DATUM_HEADLINE_MISSING);
	
	// The headline may sit in the primary tag instead
	datum_test(datum_coinbaser_headline_status("Catbus", "", headline, hl) == DATUM_HEADLINE_PRESENT);
	
	// The 0x0F separator sits between the tags, so a headline split across them is not contiguous
	datum_test(datum_coinbaser_headline_status("Cat", "bus", headline, hl) == DATUM_HEADLINE_MISSING);
	
	// 60 + 24 = 84: fits, headline at the end of the secondary tag survives
	datum_test(datum_coinbaser_headline_status(p60, "123456789012345678Catbus", headline, hl) == DATUM_HEADLINE_PRESENT);
	
	// 60 + 25 = 85: the config check allows up to 88, the coinbaser cuts one byte off the secondary
	datum_test(datum_coinbaser_headline_status(p60, "1234567890123456789Catbus", headline, hl) == DATUM_HEADLINE_TRIMMED);
	
	// 60 + 28 = 88: still allowed by the config check, four bytes cut
	datum_test(datum_coinbaser_headline_status(p60, "1234567890123456789012Catbus", headline, hl) == DATUM_HEADLINE_TRIMMED);
	
	// An empty headline matches anything, as std::search does
	datum_test(datum_coinbaser_headline_status("x", "y", headline, 0) == DATUM_HEADLINE_PRESENT);
}

void datum_coinbaser_tests(void) {
	datum_coinbaser_tests_layout();
	datum_coinbaser_tests_headline();
}
