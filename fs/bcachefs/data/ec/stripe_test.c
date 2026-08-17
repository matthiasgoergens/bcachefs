// SPDX-License-Identifier: GPL-2.0
#include <kunit/test.h>

#include "bcachefs.h"
#include "data/ec/trigger.h"
#include "btree/bkey_types.h"
#include "util/printbuf.h"

/*
 * Layout notes (measured, see kunit run logs): struct bch_val is
 * zero-sized, so value fields start at value offset 0 — ptrs[] at 8,
 * sizeof(struct bch_stripe) == 8, sizeof(struct bch_extent_ptr) == 8.
 */
struct stripe_test_key {
	struct bkey_i	k;
	u64		v[4];
};

static void stripe_to_text_truncated_test(struct kunit *test)
{
	struct stripe_test_key buf;
	struct printbuf out = PRINTBUF;

	/* Truncated key headers and truncated values must print an
	 * invalid marker, not read past the key: */
	for (unsigned u64s = 0; u64s < BKEY_U64s + 1; u64s++) {
		memset(&buf, 0, sizeof(buf));
		buf.k.k.u64s = u64s;
		buf.k.k.type = KEY_TYPE_stripe;

		bch2_stripe_to_text(&out, NULL, bkey_i_to_s_c(&buf.k));
		KUNIT_EXPECT_NOT_NULL_MSG(test, strstr(out.buf, "(invalid"), out.buf ?: "(null)");

		printbuf_reset(&out);
	}

	printbuf_exit(&out);
}

static void stripe_to_text_valid_null_fs_test(struct kunit *test)
{
	struct stripe_test_key buf = {};
	struct bch_stripe *s = (void *) buf.v;
	struct printbuf out = PRINTBUF;

	/* A well-formed single-block stripe must print with a NULL fs
	 * (the disk_label branch is skipped when c is NULL): */
	buf.k.k.u64s = BKEY_U64s + 2;
	buf.k.k.type = KEY_TYPE_stripe;
	s->nr_blocks = 1;
	s->nr_redundant = 0;
	s->disk_label = 1;

	bch2_stripe_to_text(&out, NULL, bkey_i_to_s_c(&buf.k));
	KUNIT_EXPECT_NOT_NULL_MSG(test, strstr(out.buf, "algo"), out.buf ?: "(null)");
	KUNIT_EXPECT_NULL_MSG(test, strstr(out.buf, "(invalid"), out.buf ?: "(null)");

	printbuf_exit(&out);
}

static void stripe_to_text_inflated_blocks_test(struct kunit *test)
{
	struct stripe_test_key buf = {};
	struct bch_stripe *s = (void *) buf.v;
	struct printbuf out = PRINTBUF;

	/* nr_blocks claims far more pointers than the value holds; the
	 * ptr loop's bkey_val_end() bound must stop it: */
	buf.k.k.u64s = BKEY_U64s + 2;
	buf.k.k.type = KEY_TYPE_stripe;
	s->nr_blocks = 8;
	s->nr_redundant = 0;

	bch2_stripe_to_text(&out, NULL, bkey_i_to_s_c(&buf.k));
	KUNIT_EXPECT_NOT_NULL_MSG(test, strstr(out.buf, "algo"), out.buf ?: "(null)");

	printbuf_exit(&out);
}

static struct kunit_case stripe_test_cases[] = {
	KUNIT_CASE(stripe_to_text_truncated_test),
	KUNIT_CASE(stripe_to_text_valid_null_fs_test),
	KUNIT_CASE(stripe_to_text_inflated_blocks_test),
	{}
};

static struct kunit_suite stripe_test_suite = {
	.name		= "stripe tests",
	.test_cases	= stripe_test_cases
};

kunit_test_suite(stripe_test_suite);

MODULE_AUTHOR("Matthias Goergens");
MODULE_DESCRIPTION("bcachefs filesystem stripe unit tests");
MODULE_LICENSE("GPL");
