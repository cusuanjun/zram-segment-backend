// SPDX-License-Identifier: GPL-2.0-only
/*
 * In-memory transport tests: no device, swap setup or real I/O is required.
 * Built-in only so test calls can use the backend's non-exported entry points.
 */
#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "zram_drv.h"
#include "zram_backend.h"

#define TEST_BLOCK_SIZE 4096
#define TEST_SEGMENT_BYTES (4 * PAGE_SIZE)
#define TEST_SEGMENTS 2

struct backend_fixture {
	struct zram zram;
	u8 *storage;
	unsigned int writes;
	unsigned int last_write_bytes;
	unsigned int fail_write_at;
	struct page *first_write_page;
	bool buffer_changed;
	bool fail_write;
	bool fail_read;
};

static int memory_io(void *private, u64 offset, struct page **pages,
		     unsigned int nr_pages, unsigned int bytes, bool write)
{
	struct backend_fixture *fixture = private;
	unsigned int i, remaining = bytes;
	u64 capacity = TEST_SEGMENTS * TEST_SEGMENT_BYTES;

	if (!IS_ALIGNED(offset, TEST_BLOCK_SIZE) ||
	    !IS_ALIGNED(bytes, TEST_BLOCK_SIZE) ||
	    offset > capacity || bytes > capacity - offset)
		return -EINVAL;
	if (write) {
		fixture->writes++;
		fixture->last_write_bytes = bytes;
		if (!fixture->first_write_page)
			fixture->first_write_page = pages[0];
		else if (fixture->first_write_page != pages[0])
			fixture->buffer_changed = true;
		if (fixture->fail_write || fixture->writes == fixture->fail_write_at) {
			/* Model a prefix that reached storage before an I/O error. */
			memcpy(fixture->storage + offset, page_address(pages[0]), 128);
			return -EIO;
		}
	} else if (fixture->fail_read) {
		return -EIO;
	}
	for (i = 0; i < nr_pages; i++) {
		unsigned int count = min_t(unsigned int, remaining, PAGE_SIZE);

		if (write)
			memcpy(fixture->storage + offset, page_address(pages[i]), count);
		else
			memcpy(page_address(pages[i]), fixture->storage + offset, count);
		offset += count;
		remaining -= count;
	}
	return remaining ? -EIO : 0;
}

static int backend_test_init(struct kunit *test)
{
	struct backend_fixture *fixture;
	int ret;

	fixture = kunit_kzalloc(test, sizeof(*fixture), GFP_KERNEL);
	if (!fixture)
		return -ENOMEM;
	fixture->storage = kunit_kzalloc(test, TEST_SEGMENTS * TEST_SEGMENT_BYTES, GFP_KERNEL);
	if (!fixture->storage)
		return -ENOMEM;
	init_rwsem(&fixture->zram.init_lock);
	down_write(&fixture->zram.init_lock);
	ret = zram_backend_init(&fixture->zram);
	if (!ret)
		ret = zram_backend_test_configure(&fixture->zram, TEST_SEGMENT_BYTES,
				TEST_SEGMENTS, TEST_BLOCK_SIZE, memory_io, fixture);
	if (ret)
		zram_backend_destroy(&fixture->zram);
	up_write(&fixture->zram.init_lock);
	if (ret)
		return ret;
	test->priv = fixture;
	down_read(&fixture->zram.init_lock);
	return 0;
}

static void backend_test_exit(struct kunit *test)
{
	struct backend_fixture *fixture = test->priv;

	up_read(&fixture->zram.init_lock);
	down_write(&fixture->zram.init_lock);
	zram_backend_destroy(&fixture->zram);
	up_write(&fixture->zram.init_lock);
}

static void packed_records_roundtrip(struct kunit *test)
{
	struct backend_fixture *fixture = test->priv;
	struct zram_backend_input inputs[3] = {};
	struct zram_backend_record_info info;
	struct zram_backend_stats stats;
	unsigned int lengths[] = { 1200, 1800, 1600 };
	u64 ids[3], offset = 0;
	u8 *output;
	unsigned int i, j;

	output = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, output);
	for (i = 0; i < ARRAY_SIZE(inputs); i++) {
		u8 *data = kunit_kmalloc(test, lengths[i], GFP_KERNEL);

		KUNIT_ASSERT_NOT_NULL(test, data);
		memset(data, i + 1, lengths[i]);
		inputs[i].owner.id = 1;
		inputs[i].owner.generation = 7;
		inputs[i].data = data;
		inputs[i].len = lengths[i];
		inputs[i].format = ZRAM_BACKEND_COMPRESSED;
	}
	KUNIT_ASSERT_EQ(test, zram_backend_write_batch(&fixture->zram, inputs, 3, ids), 0);
	KUNIT_EXPECT_EQ(test, fixture->writes, 1U);
	KUNIT_EXPECT_EQ(test, fixture->last_write_bytes, 8192U);
	for (i = 0; i < ARRAY_SIZE(inputs); i++) {
		KUNIT_ASSERT_EQ(test, zram_backend_read(&fixture->zram, ids[i], output,
						      PAGE_SIZE, &info), 0);
		KUNIT_EXPECT_EQ(test, info.location.offset, offset);
		KUNIT_EXPECT_EQ(test, info.location.payload_len, lengths[i]);
		KUNIT_EXPECT_EQ(test, info.owner.id, (u64)1);
		KUNIT_EXPECT_EQ(test, memcmp(output, inputs[i].data, lengths[i]), 0);
		offset += lengths[i];
	}
	for (j = 4600; j < 8192; j++)
		KUNIT_EXPECT_EQ(test, fixture->storage[j], (u8)0);
	KUNIT_ASSERT_EQ(test, zram_backend_get_stats(&fixture->zram, &stats), 0);
	KUNIT_EXPECT_EQ(test, stats.live_bytes, (u64)4600);
	KUNIT_EXPECT_EQ(test, stats.padding_bytes, (u64)3592);
	KUNIT_EXPECT_EQ(test, stats.free_segments, (u64)1);
	KUNIT_ASSERT_EQ(test, zram_backend_release(&fixture->zram, ids[1]), 0);
	KUNIT_ASSERT_EQ(test, zram_backend_get_stats(&fixture->zram, &stats), 0);
	KUNIT_EXPECT_EQ(test, stats.live_bytes, (u64)2800);
	KUNIT_EXPECT_EQ(test, stats.invalid_bytes, (u64)1800);
	KUNIT_EXPECT_EQ(test, stats.padding_bytes, (u64)3592);
	KUNIT_EXPECT_EQ(test, stats.free_segments, (u64)1);
	KUNIT_ASSERT_EQ(test, zram_backend_read(&fixture->zram, ids[2], output,
					      PAGE_SIZE, &info), 0);
	KUNIT_EXPECT_EQ(test, memcmp(output, inputs[2].data, lengths[2]), 0);
}

static void owner_validation(struct kunit *test)
{
	struct backend_fixture *fixture = test->priv;
	u8 data = 1;
	struct zram_backend_input inputs[2] = {
		{ .owner = { 1, 1 }, .data = &data, .len = 1 },
		{ .owner = { 2, 1 }, .data = &data, .len = 1 },
	};
	u64 ids[2] = { 9, 9 };

	KUNIT_EXPECT_EQ(test, zram_backend_write_batch(&fixture->zram, inputs, 2, ids),
			-EXDEV);
	KUNIT_EXPECT_EQ(test, ids[0], (u64)0);
	KUNIT_EXPECT_EQ(test, ids[1], (u64)0);
	inputs[1].owner.id = 1;
	inputs[1].owner.generation = 2;
	KUNIT_EXPECT_EQ(test, zram_backend_write_batch(&fixture->zram, inputs, 2, ids),
			-EXDEV);
	inputs[0].owner.id = 0;
	KUNIT_EXPECT_EQ(test, zram_backend_write_batch(&fixture->zram, inputs, 1, ids),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, fixture->writes, 0U);
}

static void failed_batch_is_not_published(struct kunit *test)
{
	struct backend_fixture *fixture = test->priv;
	struct zram_backend_record_info info;
	struct zram_backend_stats stats;
	u8 data[100], output[100];
	struct zram_backend_input input = {
		.owner = { 1, 1 }, .data = data, .len = sizeof(data),
	};
	u64 first, failed = 99, next;

	memset(data, 0x5a, sizeof(data));
	KUNIT_ASSERT_EQ(test, zram_backend_write_batch(&fixture->zram, &input, 1, &first), 0);
	fixture->fail_write = true;
	KUNIT_EXPECT_EQ(test, zram_backend_write_batch(&fixture->zram, &input, 1, &failed),
			-EIO);
	KUNIT_EXPECT_EQ(test, failed, (u64)0);
	fixture->fail_write = false;
	KUNIT_ASSERT_EQ(test, zram_backend_get_stats(&fixture->zram, &stats), 0);
	KUNIT_EXPECT_EQ(test, stats.records, (u64)1);
	KUNIT_EXPECT_EQ(test, stats.failed_bytes, (u64)TEST_BLOCK_SIZE);
	KUNIT_ASSERT_EQ(test, zram_backend_read(&fixture->zram, first, output,
					      sizeof(output), &info), 0);
	KUNIT_EXPECT_EQ(test, memcmp(data, output, sizeof(data)), 0);
	KUNIT_ASSERT_EQ(test, zram_backend_write_batch(&fixture->zram, &input, 1, &next), 0);
	KUNIT_ASSERT_EQ(test, zram_backend_read(&fixture->zram, next, output,
					      sizeof(output), &info), 0);
	KUNIT_EXPECT_EQ(test, info.location.offset, (u64)(2 * TEST_BLOCK_SIZE));
}

static void empty_failed_segment_is_reusable(struct kunit *test)
{
	struct backend_fixture *fixture = test->priv;
	struct zram_backend_stats stats;
	u8 data = 1;
	struct zram_backend_input input = {
		.owner = { 1, 1 }, .data = &data, .len = 1,
	};
	u64 id;

	fixture->fail_write = true;
	KUNIT_EXPECT_EQ(test, zram_backend_write_batch(&fixture->zram, &input, 1, &id), -EIO);
	KUNIT_ASSERT_EQ(test, zram_backend_get_stats(&fixture->zram, &stats), 0);
	KUNIT_EXPECT_EQ(test, stats.free_segments, (u64)TEST_SEGMENTS);
	KUNIT_EXPECT_EQ(test, stats.records, (u64)0);
	fixture->fail_write = false;
	KUNIT_EXPECT_EQ(test, zram_backend_write_batch(&fixture->zram, &input, 1, &id), 0);
}

static void release_reuse_and_read_error(struct kunit *test)
{
	struct backend_fixture *fixture = test->priv;
	struct zram_backend_record_info before, after;
	struct zram_backend_stats stats;
	u8 data[100], output[100];
	struct zram_backend_input input = {
		.owner = { 1, 1 }, .data = data, .len = sizeof(data),
	};
	u64 first, second;

	memset(data, 0x11, sizeof(data));
	KUNIT_ASSERT_EQ(test, zram_backend_write_batch(&fixture->zram, &input, 1, &first), 0);
	KUNIT_ASSERT_EQ(test, zram_backend_read(&fixture->zram, first, output,
					      sizeof(output), &before), 0);
	memset(output, 0x77, sizeof(output));
	fixture->fail_read = true;
	KUNIT_EXPECT_EQ(test, zram_backend_read(&fixture->zram, first, output,
					      sizeof(output), &after), -EIO);
	KUNIT_EXPECT_EQ(test, output[0], (u8)0x77);
	fixture->fail_read = false;
	KUNIT_EXPECT_EQ(test, zram_backend_read(&fixture->zram, first, output, 1, &after),
			-ENOBUFS);
	KUNIT_ASSERT_EQ(test, zram_backend_release(&fixture->zram, first), 0);
	KUNIT_EXPECT_EQ(test, zram_backend_release(&fixture->zram, first), -ENOENT);
	KUNIT_EXPECT_EQ(test, zram_backend_read(&fixture->zram, first, output,
					      sizeof(output), &after), -ENOENT);
	KUNIT_ASSERT_EQ(test, zram_backend_get_stats(&fixture->zram, &stats), 0);
	KUNIT_EXPECT_EQ(test, stats.free_segments, (u64)TEST_SEGMENTS);
	input.owner.id = 2;
	KUNIT_ASSERT_EQ(test, zram_backend_write_batch(&fixture->zram, &input, 1, &second), 0);
	KUNIT_ASSERT_EQ(test, zram_backend_read(&fixture->zram, second, output,
					      sizeof(output), &after), 0);
	KUNIT_EXPECT_NE(test, first, second);
	KUNIT_EXPECT_EQ(test, before.location.segment_id, after.location.segment_id);
	KUNIT_EXPECT_EQ(test, after.location.segment_generation,
			before.location.segment_generation + 1);
	KUNIT_EXPECT_EQ(test, after.owner.id, (u64)2);
}

static void owner_isolation_and_capacity(struct kunit *test)
{
	struct backend_fixture *fixture = test->priv;
	struct zram_backend_record_info first_info, second_info;
	u8 data = 1, output;
	struct zram_backend_input input = {
		.owner = { 1, 1 }, .data = &data, .len = 1,
	};
	u64 first, second, third;

	KUNIT_ASSERT_EQ(test, zram_backend_write_batch(&fixture->zram, &input, 1, &first), 0);
	input.owner.id = 2;
	KUNIT_ASSERT_EQ(test, zram_backend_write_batch(&fixture->zram, &input, 1, &second), 0);
	KUNIT_ASSERT_EQ(test, zram_backend_read(&fixture->zram, first, &output, 1,
					      &first_info), 0);
	KUNIT_ASSERT_EQ(test, zram_backend_read(&fixture->zram, second, &output, 1,
					      &second_info), 0);
	KUNIT_EXPECT_NE(test, first_info.location.segment_id, second_info.location.segment_id);
	input.owner.id = 3;
	KUNIT_EXPECT_EQ(test, zram_backend_write_batch(&fixture->zram, &input, 1, &third),
			-ENOSPC);
	KUNIT_EXPECT_EQ(test, third, (u64)0);
	KUNIT_EXPECT_EQ(test, fixture->writes, 2U);
}


static void page_crossing_and_raw_format(struct kunit *test)
{
	struct backend_fixture *fixture = test->priv;
	struct zram_backend_record_info info;
	struct zram_backend_input inputs[5] = {};
	u8 *data, *output;
	u64 ids[5];
	unsigned int i, length = PAGE_SIZE / 2 + 1;
	unsigned int writes;

	data = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	output = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, data);
	KUNIT_ASSERT_NOT_NULL(test, output);
	for (i = 0; i < PAGE_SIZE; i++)
		data[i] = (u8)i;
	for (i = 0; i < ARRAY_SIZE(inputs); i++) {
		inputs[i].owner.id = 1;
		inputs[i].data = data;
		inputs[i].len = length;
	}
	KUNIT_ASSERT_EQ(test, zram_backend_write_batch(&fixture->zram, inputs, 3, ids), 0);
	/* Record 1 crosses a host PAGE_SIZE boundary, including on 64 KiB kernels. */
	KUNIT_ASSERT_EQ(test, zram_backend_read(&fixture->zram, ids[1], output,
					      PAGE_SIZE, &info), 0);
	KUNIT_EXPECT_EQ(test, info.location.offset, (u64)length);
	KUNIT_EXPECT_EQ(test, memcmp(data, output, length), 0);

	inputs[0].format = ZRAM_BACKEND_RAW;
	inputs[0].len = PAGE_SIZE - 1;
	writes = fixture->writes;
	KUNIT_EXPECT_EQ(test, zram_backend_write_batch(&fixture->zram, inputs, 1, ids),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, fixture->writes, writes);
	inputs[0].len = PAGE_SIZE;
	KUNIT_ASSERT_EQ(test, zram_backend_write_batch(&fixture->zram, inputs, 1, ids), 0);
	KUNIT_ASSERT_EQ(test, zram_backend_read(&fixture->zram, ids[0], output,
					      PAGE_SIZE, &info), 0);
	KUNIT_EXPECT_EQ(test, info.format, ZRAM_BACKEND_RAW);
	KUNIT_EXPECT_EQ(test, memcmp(data, output, PAGE_SIZE), 0);

	for (i = 0; i < ARRAY_SIZE(inputs); i++) {
		inputs[i].len = PAGE_SIZE;
		inputs[i].format = ZRAM_BACKEND_RAW;
	}
	writes = fixture->writes;
	KUNIT_EXPECT_EQ(test, zram_backend_write_batch(&fixture->zram, inputs, 5, ids),
			-E2BIG);
	KUNIT_EXPECT_EQ(test, fixture->writes, writes);
}

/*
 * A window is grouped by owner, but returned IDs retain original input order.
 * Tiny B input follows A's full-page write to catch stale cross-owner padding.
 */
static void window_groups_and_reuses_buffer(struct kunit *test)
{
	struct backend_fixture *fixture = test->priv;
	struct zram_backend_input inputs[3] = {};
	struct zram_backend_record_info a, b, c;
	struct zram_backend_stats stats;
	u8 *data, *output;
	u64 ids[3], b_start;
	unsigned int i;

	data = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	output = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, data);
	KUNIT_ASSERT_NOT_NULL(test, output);
	memset(data, 0xa5, PAGE_SIZE);
	for (i = 0; i < 3; i++) {
		inputs[i].owner.id = i == 1 ? 2 : 1;
		inputs[i].owner.generation = 1;
		inputs[i].data = data;
		inputs[i].len = i == 1 ? 1 : PAGE_SIZE / 2;
	}
	KUNIT_ASSERT_EQ(test, zram_backend_write_records(&fixture->zram, inputs, 3, ids), 0);
	KUNIT_EXPECT_EQ(test, fixture->writes, 2U);
	KUNIT_EXPECT_FALSE(test, fixture->buffer_changed);
	KUNIT_ASSERT_EQ(test, zram_backend_read(&fixture->zram, ids[0], output,
					      PAGE_SIZE, &a), 0);
	KUNIT_EXPECT_EQ(test, memcmp(data, output, inputs[0].len), 0);
	KUNIT_ASSERT_EQ(test, zram_backend_read(&fixture->zram, ids[1], output,
					      PAGE_SIZE, &b), 0);
	KUNIT_EXPECT_EQ(test, output[0], data[0]);
	KUNIT_ASSERT_EQ(test, zram_backend_read(&fixture->zram, ids[2], output,
					      PAGE_SIZE, &c), 0);
	KUNIT_EXPECT_EQ(test, memcmp(data, output, inputs[2].len), 0);
	KUNIT_EXPECT_EQ(test, a.location.segment_id, c.location.segment_id);
	KUNIT_EXPECT_NE(test, a.location.segment_id, b.location.segment_id);
	KUNIT_EXPECT_EQ(test, c.location.offset, (u64)(PAGE_SIZE / 2));
	b_start = b.location.segment_id * TEST_SEGMENT_BYTES;
	/* The second owner's padding must not expose the first owner's bytes. */
	for (i = 1; i < TEST_BLOCK_SIZE; i++)
		KUNIT_EXPECT_EQ(test, fixture->storage[b_start + i], (u8)0);
	KUNIT_ASSERT_EQ(test, zram_backend_get_stats(&fixture->zram, &stats), 0);
	KUNIT_EXPECT_EQ(test, stats.write_buffer_bytes, (u64)TEST_SEGMENT_BYTES);
	KUNIT_EXPECT_EQ(test, stats.open_tail_bytes,
			(u64)(2 * TEST_SEGMENT_BYTES - PAGE_SIZE - TEST_BLOCK_SIZE));
}

/* Fill an old tail with the first records, instead of abandoning it. */
static void window_splits_at_segment_tail(struct kunit *test)
{
	struct backend_fixture *fixture = test->priv;
	struct zram_backend_input inputs[3] = {};
	struct zram_backend_record_info old, first, last;
	struct zram_backend_stats stats;
	u8 *data, *output;
	u64 seed_ids[3], ids[3];
	unsigned int i;

	data = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	output = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, data);
	KUNIT_ASSERT_NOT_NULL(test, output);
	memset(data, 0x73, PAGE_SIZE);
	for (i = 0; i < 3; i++) {
		inputs[i].owner.id = 1;
		inputs[i].data = data;
		inputs[i].len = PAGE_SIZE;
	}
	KUNIT_ASSERT_EQ(test, zram_backend_write_batch(&fixture->zram, inputs, 3, seed_ids), 0);
	KUNIT_ASSERT_EQ(test, zram_backend_read(&fixture->zram, seed_ids[0], output,
					      PAGE_SIZE, &old), 0);
	/* 1/4 + 1/2 fits the old one-page tail; the final 1/2 moves to a new segment. */
	inputs[0].len = PAGE_SIZE / 4;
	inputs[1].len = PAGE_SIZE / 2;
	inputs[2].len = PAGE_SIZE / 2;
	KUNIT_ASSERT_EQ(test, zram_backend_write_records(&fixture->zram, inputs, 3, ids), 0);
	KUNIT_EXPECT_EQ(test, fixture->writes, 3U);
	KUNIT_EXPECT_FALSE(test, fixture->buffer_changed);
	KUNIT_ASSERT_EQ(test, zram_backend_read(&fixture->zram, ids[0], output,
					      PAGE_SIZE, &first), 0);
	KUNIT_EXPECT_EQ(test, first.location.segment_id, old.location.segment_id);
	KUNIT_EXPECT_EQ(test, first.location.offset, (u64)(3 * PAGE_SIZE));
	KUNIT_ASSERT_EQ(test, zram_backend_read(&fixture->zram, ids[2], output,
					      PAGE_SIZE, &last), 0);
	KUNIT_EXPECT_NE(test, last.location.segment_id, old.location.segment_id);
	KUNIT_EXPECT_EQ(test, last.location.offset, (u64)0);
	KUNIT_EXPECT_EQ(test, memcmp(data, output, inputs[2].len), 0);
	KUNIT_ASSERT_EQ(test, zram_backend_get_stats(&fixture->zram, &stats), 0);
	KUNIT_EXPECT_EQ(test, stats.records, (u64)6);
}

/* Old one-shot API leaves an insufficient tail available for a smaller write. */
static void older_tail_remains_usable(struct kunit *test)
{
	struct backend_fixture *fixture = test->priv;
	struct zram_backend_input inputs[3] = {};
	struct zram_backend_record_info old, newer, reused;
	u8 *data, *output;
	u64 ids[3], more[2], small;
	unsigned int i;

	data = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL);
	output = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, data);
	KUNIT_ASSERT_NOT_NULL(test, output);
	for (i = 0; i < 3; i++) {
		inputs[i].owner.id = 1;
		inputs[i].data = data;
		inputs[i].len = PAGE_SIZE;
	}
	KUNIT_ASSERT_EQ(test, zram_backend_write_batch(&fixture->zram, inputs, 3, ids), 0);
	KUNIT_ASSERT_EQ(test, zram_backend_write_batch(&fixture->zram, inputs, 2, more), 0);
	KUNIT_ASSERT_EQ(test, zram_backend_write_batch(&fixture->zram, inputs, 1, &small), 0);
	KUNIT_ASSERT_EQ(test, zram_backend_read(&fixture->zram, ids[0], output,
					      PAGE_SIZE, &old), 0);
	KUNIT_ASSERT_EQ(test, zram_backend_read(&fixture->zram, more[0], output,
					      PAGE_SIZE, &newer), 0);
	KUNIT_ASSERT_EQ(test, zram_backend_read(&fixture->zram, small, output,
					      PAGE_SIZE, &reused), 0);
	KUNIT_EXPECT_NE(test, old.location.segment_id, newer.location.segment_id);
	KUNIT_EXPECT_EQ(test, old.location.segment_id, reused.location.segment_id);
	KUNIT_EXPECT_EQ(test, reused.location.offset, (u64)(3 * PAGE_SIZE));
}

/* Success IDs can be non-contiguous because owner A is processed before B. */
static void window_failure_preserves_earlier_ids(struct kunit *test)
{
	struct backend_fixture *fixture = test->priv;
	struct zram_backend_record_info info;
	struct zram_backend_stats stats;
	u8 data = 0x62, output;
	struct zram_backend_input inputs[3] = {
		{ .owner = { 1, 1 }, .data = &data, .len = 1 },
		{ .owner = { 2, 1 }, .data = &data, .len = 1 },
		{ .owner = { 1, 1 }, .data = &data, .len = 1 },
	};
	u64 ids[3];

	fixture->fail_write_at = 2;
	KUNIT_EXPECT_EQ(test, zram_backend_write_records(&fixture->zram, inputs, 3, ids),
			-EIO);
	KUNIT_EXPECT_NE(test, ids[0], (u64)0);
	KUNIT_EXPECT_EQ(test, ids[1], (u64)0);
	KUNIT_EXPECT_NE(test, ids[2], (u64)0);
	KUNIT_ASSERT_EQ(test, zram_backend_read(&fixture->zram, ids[2], &output, 1, &info), 0);
	KUNIT_EXPECT_EQ(test, output, data);
	KUNIT_ASSERT_EQ(test, zram_backend_get_stats(&fixture->zram, &stats), 0);
	KUNIT_EXPECT_EQ(test, stats.records, (u64)2);
	KUNIT_EXPECT_EQ(test, stats.free_segments, (u64)1);
	/* Retry only the zero-ID record, not the whole original window. */
	fixture->fail_write_at = 0;
	KUNIT_ASSERT_EQ(test, zram_backend_write_records(&fixture->zram, &inputs[1], 1,
						       &ids[1]), 0);
	KUNIT_ASSERT_EQ(test, zram_backend_release(&fixture->zram, ids[0]), 0);
	KUNIT_ASSERT_EQ(test, zram_backend_release(&fixture->zram, ids[1]), 0);
	KUNIT_ASSERT_EQ(test, zram_backend_release(&fixture->zram, ids[2]), 0);
	KUNIT_ASSERT_EQ(test, zram_backend_get_stats(&fixture->zram, &stats), 0);
	KUNIT_EXPECT_EQ(test, stats.free_segments, (u64)2);
}

static void window_capacity_and_validation(struct kunit *test)
{
	struct backend_fixture *fixture = test->priv;
	struct zram_backend_input inputs[9] = {};
	struct zram_backend_stats stats;
	u8 *data = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL);
	u64 ids[9];
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, data);
	for (i = 0; i < ARRAY_SIZE(inputs); i++) {
		inputs[i].owner.id = 1;
		inputs[i].data = data;
		inputs[i].len = PAGE_SIZE;
		inputs[i].format = ZRAM_BACKEND_RAW;
	}
	inputs[8].owner.id = 0;
	KUNIT_EXPECT_EQ(test, zram_backend_write_records(&fixture->zram, inputs, 9, ids),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, fixture->writes, 0U);
	for (i = 0; i < ARRAY_SIZE(inputs); i++)
		KUNIT_EXPECT_EQ(test, ids[i], (u64)0);
	inputs[8].owner.id = 1;
	KUNIT_EXPECT_EQ(test, zram_backend_write_records(&fixture->zram, inputs, 9, ids),
			-ENOSPC);
	for (i = 0; i < 8; i++)
		KUNIT_EXPECT_NE(test, ids[i], (u64)0);
	KUNIT_EXPECT_EQ(test, ids[8], (u64)0);
	KUNIT_EXPECT_EQ(test, fixture->writes, 2U);
	KUNIT_ASSERT_EQ(test, zram_backend_get_stats(&fixture->zram, &stats), 0);
	KUNIT_EXPECT_EQ(test, stats.records, (u64)8);
	KUNIT_EXPECT_EQ(test, stats.open_tail_bytes, (u64)0);
}

static struct kunit_case backend_test_cases[] = {
	KUNIT_CASE(packed_records_roundtrip),
	KUNIT_CASE(owner_validation),
	KUNIT_CASE(failed_batch_is_not_published),
	KUNIT_CASE(empty_failed_segment_is_reusable),
	KUNIT_CASE(release_reuse_and_read_error),
	KUNIT_CASE(owner_isolation_and_capacity),
	KUNIT_CASE(page_crossing_and_raw_format),
	KUNIT_CASE(window_groups_and_reuses_buffer),
	KUNIT_CASE(window_splits_at_segment_tail),
	KUNIT_CASE(older_tail_remains_usable),
	KUNIT_CASE(window_failure_preserves_earlier_ids),
	KUNIT_CASE(window_capacity_and_validation),
	{}
};

static struct kunit_suite backend_test_suite = {
	.name = "zram-segment-backend",
	.init = backend_test_init,
	.exit = backend_test_exit,
	.test_cases = backend_test_cases,
};

kunit_test_suite(backend_test_suite);

MODULE_LICENSE("GPL");
