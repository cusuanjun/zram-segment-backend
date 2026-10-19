// SPDX-License-Identifier: GPL-2.0-only
/*
 * Bounded input windows grouped into single-owner compressed I/O batches.
 *
 * This backend is NOT connected to legacy ZRAM_WB entries. Configuration is
 * explicit and excludes its address range from the legacy bitmap allocator.
 * Metadata is volatile: no crash recovery or automatic flushing/GC scheduling.
 * A single mutex serializes allocation, synchronous I/O and release initially.
 */
#include <linux/bio.h>
#include <linux/bitmap.h>
#include <linux/blkdev.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/list.h>
#include <linux/log2.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include "zram_drv.h"
#include "zram_backend.h"

enum zram_backend_segment_state {
	ZRAM_BACKEND_SEGMENT_FREE,
	ZRAM_BACKEND_SEGMENT_OPEN,
	ZRAM_BACKEND_SEGMENT_SEALED,
};

struct zram_backend_segment {
	/* Only published live records; protected by backend->lock. */
	struct list_head record_list;
	struct zram_backend_owner owner;
	u64 generation;
	u64 start;
	u64 append_tail;
	u64 live_bytes;
	u64 invalid_bytes;
	u64 padding_bytes;
	u64 failed_bytes;
	u64 records;
	enum zram_backend_segment_state state;
};

struct zram_backend_record {
	struct list_head node; /* global ID lookup */
	struct list_head segment_node; /* owning segment's live-record index */
	u64 id;
	struct zram_backend_record_info info;
};

struct zram_backend_buffer {
	struct page **pages;
	unsigned int nr_pages;
	unsigned int bytes;
};

typedef int (*zram_backend_io_fn)(void *private, u64 offset,
				 struct page **pages, unsigned int nr_pages,
				 unsigned int bytes, bool write);

struct zram_backend {
	struct mutex lock;
	/* Allocated once at configure, reused under lock across all owners. */
	struct zram_backend_buffer write_buffer;
	struct list_head records;
	struct zram_backend_segment *segments;
	unsigned int nr_segments;
	unsigned int block_size;
	u64 segment_bytes;
	u64 next_record_id;
	/* Device/range lifetime is protected by the caller's init_lock. */
	unsigned int reserved_start;
	unsigned int reserved_pages;
	zram_backend_io_fn submit;
	void *io_private;
};

static bool same_owner(const struct zram_backend_owner *a,
		       const struct zram_backend_owner *b)
{
	return a->id == b->id && a->generation == b->generation;
}

static void buffer_free(struct zram_backend_buffer *buffer)
{
	unsigned int i;

	for (i = 0; i < buffer->nr_pages; i++)
		__free_page(buffer->pages[i]);
	kfree(buffer->pages);
	memset(buffer, 0, sizeof(*buffer));
}

static int buffer_alloc(struct zram_backend_buffer *buffer, unsigned int bytes)
{
	unsigned int i, nr_pages = DIV_ROUND_UP(bytes, PAGE_SIZE);

	memset(buffer, 0, sizeof(*buffer));
	buffer->bytes = bytes;
	buffer->pages = kcalloc(nr_pages, sizeof(*buffer->pages), GFP_NOIO);
	if (!buffer->pages) {
		memset(buffer, 0, sizeof(*buffer));
		return -ENOMEM;
	}

	for (i = 0; i < nr_pages; i++) {
		/* Lowmem pages: page_address() is valid; padding starts zeroed. */
		buffer->pages[i] = alloc_page(GFP_NOIO | __GFP_ZERO);
		if (!buffer->pages[i]) {
			buffer_free(buffer);
			return -ENOMEM;
		}
		buffer->nr_pages++;
	}
	return 0;
}

static void buffer_copy(struct zram_backend_buffer *buffer, unsigned int offset,
			void *data, unsigned int bytes, bool into_buffer)
{
	u8 *cursor = data;

	while (bytes) {
		unsigned int in_page = offset % PAGE_SIZE;
		unsigned int count = min_t(unsigned int, bytes, PAGE_SIZE - in_page);
		u8 *address = page_address(buffer->pages[offset / PAGE_SIZE]);

		if (into_buffer)
			memcpy(address + in_page, cursor, count);
		else
			memcpy(cursor, address + in_page, count);
		offset += count;
		cursor += count;
		bytes -= count;
	}
}

/* A reused buffer may contain the previous App's bytes in its new padding. */
static void buffer_zero(struct zram_backend_buffer *buffer, unsigned int offset,
			unsigned int bytes)
{
	while (bytes) {
		unsigned int in_page = offset % PAGE_SIZE;
		unsigned int count = min_t(unsigned int, bytes, PAGE_SIZE - in_page);
		u8 *address = page_address(buffer->pages[offset / PAGE_SIZE]);

		memset(address + in_page, 0, count);
		offset += count;
		bytes -= count;
	}
}

static int block_io(void *private, u64 offset, struct page **pages,
		    unsigned int nr_pages, unsigned int bytes, bool write)
{
	struct block_device *bdev = private;
	struct bio *bio;
	unsigned int i, remaining = bytes;
	int ret;

	if (nr_pages > BIO_MAX_VECS)
		return -E2BIG;

	bio = bio_alloc(bdev, nr_pages,
			write ? REQ_OP_WRITE | REQ_SYNC : REQ_OP_READ, GFP_NOIO);
	if (!bio)
		return -ENOMEM;
	bio->bi_iter.bi_sector = offset >> SECTOR_SHIFT;

	for (i = 0; i < nr_pages; i++) {
		unsigned int count = min_t(unsigned int, remaining, PAGE_SIZE);

		if (bio_add_page(bio, pages[i], count, 0) != count) {
			ret = -EIO;
			goto out;
		}
		remaining -= count;
	}
	ret = submit_bio_wait(bio);
out:
	bio_put(bio);
	return ret;
}

static int setup_segments(struct zram_backend *backend, u64 start,
			  u64 segment_bytes, unsigned int nr_segments,
			  unsigned int block_size)
{
	unsigned int i, buffer_bytes;
	int ret;

	if (backend->segments)
		return -EBUSY;
	if (nr_segments < 2 || !segment_bytes ||
	    !IS_ALIGNED(segment_bytes, PAGE_SIZE) ||
	    !is_power_of_2(block_size) || block_size < 512 ||
	    block_size > PAGE_SIZE || !IS_ALIGNED(start, PAGE_SIZE))
		return -EINVAL;

	backend->segments = kvcalloc(nr_segments, sizeof(*backend->segments), GFP_NOIO);
	if (!backend->segments)
		return -ENOMEM;

	/*
	 * Policy cap, not device geometry. One shared buffer per device, not
	 * one per App. PAGE_SIZE alignment also meets supported block sizes.
	 */
	buffer_bytes = min_t(u64, segment_bytes,
			    (u64)min_t(unsigned int, ZRAM_BACKEND_MAX_BATCH_RECORDS,
				       BIO_MAX_VECS) * PAGE_SIZE);
	ret = buffer_alloc(&backend->write_buffer, buffer_bytes);
	if (ret) {
		kvfree(backend->segments);
		backend->segments = NULL;
		return ret;
	}
	backend->nr_segments = nr_segments;
	backend->block_size = block_size;
	backend->segment_bytes = segment_bytes;
	for (i = 0; i < nr_segments; i++) {
		INIT_LIST_HEAD(&backend->segments[i].record_list);
		backend->segments[i].start = start + (u64)i * segment_bytes;
	}
	return 0;
}

int zram_backend_init(struct zram *zram)
{
	struct zram_backend *backend;

	if (zram->backend)
		return -EBUSY;
	backend = kzalloc(sizeof(*backend), GFP_KERNEL);
	if (!backend)
		return -ENOMEM;
	mutex_init(&backend->lock);
	INIT_LIST_HEAD(&backend->records);
	backend->next_record_id = 1;
	zram->backend = backend;
	return 0;
}

int zram_backend_configure(struct zram *zram, u64 start, u64 bytes,
			   u64 segment_bytes)
{
	struct zram_backend *backend = zram->backend;
	u64 first, count, nr_segments, capacity;
	int ret;

	if (!backend || !zram->bdev || !zram->backing_dev || !zram->bitmap)
		return -ENODEV;
	if (!bytes || !segment_bytes || start < PAGE_SIZE ||
	    !IS_ALIGNED(start, PAGE_SIZE) || !IS_ALIGNED(bytes, PAGE_SIZE) ||
	    !IS_ALIGNED(segment_bytes, PAGE_SIZE) || bytes % segment_bytes)
		return -EINVAL;

	capacity = i_size_read(zram->backing_dev->f_mapping->host);
	if (start > capacity || bytes > capacity - start)
		return -EINVAL;
	first = start >> PAGE_SHIFT;
	count = bytes >> PAGE_SHIFT;
	nr_segments = bytes / segment_bytes;
	/* bitmap_set/clear use a bounded bit count. */
	if (first > UINT_MAX || count > INT_MAX || first + count > UINT_MAX ||
	    first + count > zram->nr_pages || nr_segments > UINT_MAX)
		return -E2BIG;

	mutex_lock(&backend->lock);
	if (backend->segments) {
		ret = -EBUSY;
		goto out;
	}
	if (find_next_bit(zram->bitmap, first + count, first) < first + count) {
		ret = -EBUSY;
		goto out;
	}
	ret = setup_segments(backend, start, segment_bytes, nr_segments,
			     bdev_logical_block_size(zram->bdev));
	if (ret)
		goto out;

	/*
	 * init_lock is held for write: no legacy allocator can race the claim.
	 * Reservation bits are not legacy live-page statistics.
	 */
	bitmap_set(zram->bitmap, first, count);
	backend->reserved_start = first;
	backend->reserved_pages = count;
	backend->submit = block_io;
	backend->io_private = zram->bdev;
out:
	mutex_unlock(&backend->lock);
	return ret;
}

static struct zram_backend_segment *
select_segment(struct zram_backend *backend, const struct zram_backend_owner *owner,
	       unsigned int bytes)
{
	struct zram_backend_segment *free_segment = NULL;
	unsigned int i, usable_free = 0;

	for (i = 0; i < backend->nr_segments; i++) {
		struct zram_backend_segment *segment = &backend->segments[i];

		if (segment->state == ZRAM_BACKEND_SEGMENT_FREE) {
			if (segment->generation != U64_MAX) {
				usable_free++;
				if (!free_segment)
					free_segment = segment;
			}
			continue;
		}
		if (segment->state != ZRAM_BACKEND_SEGMENT_OPEN ||
		    !same_owner(&segment->owner, owner))
			continue;
		if (bytes <= backend->segment_bytes - segment->append_tail)
			return segment;
	}

	/* Ordinary writes may append to owned tails, but keep one GC spare. */
	if (!free_segment || usable_free <= 1)
		return NULL;
	/* Keep older non-full tails OPEN for later smaller records of this owner. */
	free_segment->owner = *owner;
	free_segment->generation++;
	free_segment->state = ZRAM_BACKEND_SEGMENT_OPEN;
	return free_segment;
}

/*
 * Publish/remove both indexes and their accounting under backend->lock.
 * These helpers do not allocate, issue I/O or free the record. GC transfers
 * only the segment node, preserving the global ID entry.
 */
static void publish_record(struct zram_backend *backend,
			   struct zram_backend_segment *segment,
			   struct zram_backend_record *record)
{
	list_add_tail(&record->node, &backend->records);
	list_add_tail(&record->segment_node, &segment->record_list);
	segment->records++;
	segment->live_bytes += record->info.location.payload_len;
}

static void unpublish_record(struct zram_backend_segment *segment,
			     struct zram_backend_record *record)
{
	list_del(&record->node);
	list_del(&record->segment_node);
	segment->records--;
	segment->live_bytes -= record->info.location.payload_len;
	segment->invalid_bytes += record->info.location.payload_len;
}

/* The backend lock excludes all synchronous reads/writes at this point. */
static void recycle_empty_segment(struct zram_backend_segment *segment)
{
	if (segment->records)
		return;
	/* Never recycle a count-zero segment that still owns live records. */
	if (WARN_ON_ONCE(!list_empty(&segment->record_list) || segment->live_bytes))
		return;
	memset(&segment->owner, 0, sizeof(segment->owner));
	segment->append_tail = 0;
	segment->live_bytes = 0;
	segment->invalid_bytes = 0;
	segment->padding_bytes = 0;
	segment->failed_bytes = 0;
	segment->state = ZRAM_BACKEND_SEGMENT_FREE;
	/* Keep generation; the next assignment increments it. No discard here. */
}

/*
 * Validate the complete bounded window before issuing any I/O. Caller keeps
 * inputs and payload snapshots immutable until the API returns.
 */
static int validate_inputs(struct zram_backend *backend,
			   const struct zram_backend_input *inputs,
			   unsigned int nr_records, u64 *record_ids, bool single_owner)
{
	unsigned int i;

	if (!record_ids || !nr_records || nr_records > ZRAM_BACKEND_MAX_BATCH_RECORDS)
		return -EINVAL;
	memset(record_ids, 0, nr_records * sizeof(*record_ids));
	if (!backend || !inputs)
		return -EINVAL;
	for (i = 0; i < nr_records; i++) {
		if (!inputs[i].owner.id || !inputs[i].data ||
		    !inputs[i].len || inputs[i].len > PAGE_SIZE ||
		    (inputs[i].format != ZRAM_BACKEND_COMPRESSED &&
		     inputs[i].format != ZRAM_BACKEND_RAW) ||
		    (inputs[i].format == ZRAM_BACKEND_RAW && inputs[i].len != PAGE_SIZE))
			return -EINVAL;
		if (single_owner && !same_owner(&inputs[0].owner, &inputs[i].owner))
			return -EXDEV;
	}
	return 0;
}

/*
 * lock held; segment already selected. indices map this I/O back to the input
 * window. Publish this I/O's IDs only after all its bytes have been written.
 * A fresh segment selected by the caller is returned if preparation fails.
 */
static int write_one(struct zram_backend *backend,
		     struct zram_backend_segment *segment,
		     const struct zram_backend_input *inputs,
		     const unsigned int *indices, unsigned int nr_records,
		     unsigned int payload_bytes, u64 *record_ids)
{
	struct zram_backend_record *records[ZRAM_BACKEND_MAX_BATCH_RECORDS] = {};
	struct zram_backend_buffer *buffer = &backend->write_buffer;
	unsigned int i, cursor = 0;
	unsigned int io_bytes = round_up(payload_bytes, backend->block_size);
	u64 offset;
	int ret = -ENOMEM;

	if (backend->next_record_id > U64_MAX - nr_records) {
		ret = -EOVERFLOW;
		goto free_records;
	}
	if (io_bytes > buffer->bytes ||
	    io_bytes > backend->segment_bytes - segment->append_tail) {
		ret = -E2BIG;
		goto free_records;
	}
	/* Persistent record metadata still needs allocations; buffer reuse does
	 * not make this path allocation-free or guarantee low-memory progress.
	 */
	for (i = 0; i < nr_records; i++) {
		records[i] = kzalloc(sizeof(*records[i]), GFP_NOIO);
		if (!records[i])
			goto free_records;
	}
	for (i = 0; i < nr_records; i++) {
		const struct zram_backend_input *input = &inputs[indices[i]];

		buffer_copy(buffer, cursor, (void *)input->data, input->len, true);
		cursor += input->len;
	}
	buffer_zero(buffer, payload_bytes, io_bytes - payload_bytes);
	offset = segment->append_tail;
	segment->append_tail += io_bytes;
	if (segment->append_tail == backend->segment_bytes)
		segment->state = ZRAM_BACKEND_SEGMENT_SEALED;
	ret = backend->submit(backend->io_private, segment->start + offset,
			      buffer->pages, DIV_ROUND_UP(io_bytes, PAGE_SIZE),
			      io_bytes, true);
	if (ret) {
		/* Failed writes may have reached storage; never reuse a live tail. */
		segment->failed_bytes += io_bytes;
		goto free_records;
	}

	cursor = 0;
	for (i = 0; i < nr_records; i++) {
		const struct zram_backend_input *input = &inputs[indices[i]];
		struct zram_backend_record *record = records[i];

		record->id = backend->next_record_id++;
		record->info.owner = input->owner;
		record->info.format = input->format;
		record->info.location.segment_id = segment - backend->segments;
		record->info.location.segment_generation = segment->generation;
		record->info.location.offset = offset + cursor;
		record->info.location.payload_len = input->len;
		record->info.location.stored_len = input->len;
		publish_record(backend, segment, record);
		record_ids[indices[i]] = record->id;
		cursor += input->len;
	}
	segment->padding_bytes += io_bytes - payload_bytes;
	return 0;

free_records:
	for (i = 0; i < nr_records; i++)
		kfree(records[i]);
	recycle_empty_segment(segment);
	return ret;
}

int zram_backend_write_batch(struct zram *zram,
			     const struct zram_backend_input *inputs,
			     unsigned int nr_records, u64 *record_ids)
{
	struct zram_backend *backend = zram->backend;
	struct zram_backend_segment *segment;
	unsigned int indices[ZRAM_BACKEND_MAX_BATCH_RECORDS];
	unsigned int i, payload_bytes = 0, io_bytes;
	int ret;

	ret = validate_inputs(backend, inputs, nr_records, record_ids, true);
	if (ret)
		return ret;
	for (i = 0; i < nr_records; i++) {
		indices[i] = i;
		payload_bytes += inputs[i].len;
	}
	mutex_lock(&backend->lock);
	if (!backend->segments) {
		ret = -ENODEV;
		goto out;
	}
	io_bytes = round_up(payload_bytes, backend->block_size);
	if (io_bytes > backend->write_buffer.bytes) {
		ret = -E2BIG;
		goto out;
	}
	segment = select_segment(backend, &inputs[0].owner, io_bytes);
	if (!segment) {
		ret = -ENOSPC;
		goto out;
	}
	ret = write_one(backend, segment, inputs, indices, nr_records,
			payload_bytes, record_ids);
out:
	mutex_unlock(&backend->lock);
	return ret;
}

/*
 * Pure packing decision: limit is block-aligned and admits inputs[next].
 * Return the next record of this owner that did not fit, or nr_records.
 */
static unsigned int plan_batch(const struct zram_backend_input *inputs,
			       unsigned int nr_records, unsigned int next,
			       unsigned int limit, unsigned int block_size,
			       unsigned int *indices, unsigned int *count,
			       unsigned int *payload_bytes)
{
	unsigned int j;

	*count = 0;
	*payload_bytes = 0;
	for (j = next; j < nr_records; j++) {
		if (!same_owner(&inputs[next].owner, &inputs[j].owner))
			continue;
		if (round_up(*payload_bytes + inputs[j].len, block_size) > limit)
			break;
		indices[(*count)++] = j;
		*payload_bytes += inputs[j].len;
	}
	return j;
}

int zram_backend_write_records(struct zram *zram,
			       const struct zram_backend_input *inputs,
			       unsigned int nr_records, u64 *record_ids)
{
	struct zram_backend *backend = zram->backend;
	struct zram_backend_segment *segment;
	unsigned int indices[ZRAM_BACKEND_MAX_BATCH_RECORDS];
	unsigned int i, j, next, count, payload_bytes, limit;
	int ret;

	ret = validate_inputs(backend, inputs, nr_records, record_ids, false);
	if (ret)
		return ret;
	mutex_lock(&backend->lock);
	if (!backend->segments) {
		ret = -ENODEV;
		goto out;
	}
	/*
	 * Process owners by first appearance, preserving order within each owner.
	 * The window has at most 64 records: bounded scans avoid persistent,
	 * unbounded per-App queues. IDs always refer to ORIGINAL input positions.
	 */
	for (i = 0; i < nr_records; i++) {
		if (record_ids[i])
			continue;
		next = i;
		while (next < nr_records) {
			segment = select_segment(backend, &inputs[i].owner,
					round_up(inputs[next].len, backend->block_size));
			if (!segment) {
				ret = -ENOSPC;
				goto out;
			}
			limit = min_t(u64, backend->write_buffer.bytes,
				      backend->segment_bytes - segment->append_tail);
			j = plan_batch(inputs, nr_records, next, limit, backend->block_size,
				       indices, &count, &payload_bytes);
			ret = write_one(backend, segment, inputs, indices, count,
					payload_bytes, record_ids);
			if (ret)
				goto out;
			/* j is either the next unprocessed record of this owner or end. */
			next = j;
		}
	}
	ret = 0;
out:
	mutex_unlock(&backend->lock);
	return ret;
}

static struct zram_backend_record *find_record(struct zram_backend *backend, u64 id)
{
	struct zram_backend_record *record;

	list_for_each_entry(record, &backend->records, node)
		if (record->id == id)
			return record;
	return NULL;
}

int zram_backend_read(struct zram *zram, u64 record_id, void *dst,
		      size_t capacity, struct zram_backend_record_info *info)
{
	struct zram_backend *backend = zram->backend;
	struct zram_backend_record *record;
	struct zram_backend_segment *segment;
	struct zram_backend_location *location;
	struct zram_backend_buffer buffer;
	u64 start;
	unsigned int prefix, bytes;
	int ret;

	if (!backend || !dst || !info)
		return -EINVAL;
	mutex_lock(&backend->lock);
	record = find_record(backend, record_id);
	if (!record) {
		ret = -ENOENT;
		goto out;
	}
	location = &record->info.location;
	if (capacity < location->payload_len) {
		ret = -ENOBUFS;
		goto out;
	}
	segment = &backend->segments[location->segment_id];
	start = round_down(location->offset, (u64)backend->block_size);
	prefix = location->offset - start;
	bytes = round_up(prefix + location->payload_len, backend->block_size);
	ret = buffer_alloc(&buffer, bytes);
	if (ret)
		goto out;
	ret = backend->submit(backend->io_private, segment->start + start,
			      buffer.pages, buffer.nr_pages, bytes, false);
	if (!ret) {
		buffer_copy(&buffer, prefix, dst, location->payload_len, false);
		*info = record->info;
	}
	buffer_free(&buffer);
out:
	mutex_unlock(&backend->lock);
	return ret;
}

int zram_backend_release(struct zram *zram, u64 record_id)
{
	struct zram_backend *backend = zram->backend;
	struct zram_backend_record *record;
	struct zram_backend_segment *segment;
	int ret = 0;

	if (!backend)
		return -ENODEV;
	mutex_lock(&backend->lock);
	record = find_record(backend, record_id);
	if (!record) {
		ret = -ENOENT;
		goto out;
	}
	segment = &backend->segments[record->info.location.segment_id];
	unpublish_record(segment, record);
	kfree(record);
	recycle_empty_segment(segment);
out:
	mutex_unlock(&backend->lock);
	return ret;
}


/* Copy between page arrays without allocating a payload-sized snapshot. */
static void buffer_transfer(struct zram_backend_buffer *dst, unsigned int to,
			    struct zram_backend_buffer *src, unsigned int from,
			    unsigned int bytes)
{
	while (bytes) {
		unsigned int count = min_t(unsigned int, bytes,
					   PAGE_SIZE - from % PAGE_SIZE);
		void *data = (u8 *)page_address(src->pages[from / PAGE_SIZE]) +
			     from % PAGE_SIZE;

		buffer_copy(dst, to, data, count, true);
		from += count;
		to += count;
		bytes -= count;
	}
}

/* No live records reference dst until the entire copy has succeeded. */
static int gc_flush(struct zram_backend *backend,
		    struct zram_backend_segment *dst, unsigned int bytes)
{
	struct zram_backend_buffer *buffer = &backend->write_buffer;
	int ret;

	ret = backend->submit(backend->io_private, dst->start + dst->append_tail,
			      buffer->pages, DIV_ROUND_UP(bytes, PAGE_SIZE), bytes, true);
	if (!ret)
		dst->append_tail += bytes;
	return ret;
}

/* lock held; preserve the global index, ID, owner and format. */
static void transfer_record(struct zram_backend *backend,
			    struct zram_backend_segment *src,
			    struct zram_backend_segment *dst,
			    struct zram_backend_record *record, u64 offset)
{
	unsigned int bytes = record->info.location.payload_len;

	list_del(&record->segment_node);
	list_add_tail(&record->segment_node, &dst->record_list);
	src->records--;
	src->live_bytes -= bytes;
	src->invalid_bytes += bytes;
	dst->records++;
	dst->live_bytes += bytes;
	record->info.location.segment_id = dst - backend->segments;
	record->info.location.segment_generation = dst->generation;
	record->info.location.offset = offset;
}

int zram_backend_gc_segment(struct zram *zram, u64 segment_id,
			    u64 segment_generation)
{
	struct zram_backend *backend = zram->backend;
	struct zram_backend_segment *src, *dst = NULL;
	struct zram_backend_record *record, *next;
	struct zram_backend_buffer scratch;
	struct zram_backend_buffer *buffer;
	u64 cursor;
	unsigned int i, used = 0;
	int ret;

	if (!backend)
		return -ENODEV;
	mutex_lock(&backend->lock);
	if (!backend->segments) {
		ret = -ENODEV;
		goto out;
	}
	if (segment_id >= backend->nr_segments) {
		ret = -EINVAL;
		goto out;
	}
	src = &backend->segments[segment_id];
	if (src->state == ZRAM_BACKEND_SEGMENT_FREE ||
	    src->generation != segment_generation) {
		ret = -ESTALE;
		goto out;
	}
	/* Compact only when at least one logical block of tail is recovered. */
	if (round_up(src->live_bytes, (u64)backend->block_size) >= src->append_tail) {
		ret = -EALREADY;
		goto out;
	}
	/* Entire compacted payload fits in one fresh segment. No tail sharing. */
	for (i = 0; i < backend->nr_segments; i++) {
		if (backend->segments[i].state == ZRAM_BACKEND_SEGMENT_FREE &&
		    backend->segments[i].generation != U64_MAX) {
			dst = &backend->segments[i];
			break;
		}
	}
	if (!dst) {
		ret = -ENOSPC;
		goto out;
	}
	/* One <= PAGE_SIZE record can cover at most two aligned pages. */
	ret = buffer_alloc(&scratch, 2 * PAGE_SIZE);
	if (ret)
		goto out;
	dst->owner = src->owner;
	dst->generation++;
	dst->state = ZRAM_BACKEND_SEGMENT_OPEN;
	buffer = &backend->write_buffer;
	list_for_each_entry(record, &src->record_list, segment_node) {
		struct zram_backend_location *loc = &record->info.location;
		u64 start = round_down(loc->offset, (u64)backend->block_size);
		unsigned int prefix = loc->offset - start;
		unsigned int bytes = round_up(prefix + loc->payload_len, backend->block_size);
		unsigned int copied = 0;

		ret = backend->submit(backend->io_private, src->start + start,
				      scratch.pages, DIV_ROUND_UP(bytes, PAGE_SIZE),
				      bytes, false);
		if (ret)
			goto abort;
		while (copied < loc->payload_len) {
			unsigned int count = min_t(unsigned int, loc->payload_len - copied,
						   buffer->bytes - used);

			buffer_transfer(buffer, used, &scratch, prefix + copied, count);
			used += count;
			copied += count;
			if (used == buffer->bytes) {
				ret = gc_flush(backend, dst, used);
				if (ret)
					goto abort;
				used = 0;
			}
		}
	}
	if (used) {
		unsigned int bytes = round_up(used, backend->block_size);

		buffer_zero(buffer, used, bytes - used);
		ret = gc_flush(backend, dst, bytes);
		if (ret)
			goto abort;
	}
	/* All I/O succeeded. This commit cannot allocate or fail under the lock. */
	cursor = 0;
	list_for_each_entry_safe(record, next, &src->record_list, segment_node) {
		transfer_record(backend, src, dst, record, cursor);
		cursor += record->info.location.payload_len;
	}
	dst->padding_bytes = dst->append_tail - dst->live_bytes;
	if (dst->append_tail == backend->segment_bytes)
		dst->state = ZRAM_BACKEND_SEGMENT_SEALED;
	recycle_empty_segment(src);
	ret = 0;
	goto free_scratch;
abort:
	/* Synchronous I/O has ended; no mapping was published into this segment.
	 * Stale/partially written bytes are unreachable and can be overwritten.
	 */
	recycle_empty_segment(dst);
free_scratch:
	buffer_free(&scratch);
out:
	mutex_unlock(&backend->lock);
	return ret;
}

int zram_backend_get_stats(struct zram *zram, struct zram_backend_stats *stats)
{
	struct zram_backend *backend = zram->backend;
	unsigned int i;

	if (!backend || !stats)
		return -EINVAL;
	memset(stats, 0, sizeof(*stats));
	mutex_lock(&backend->lock);
	stats->write_buffer_bytes = backend->write_buffer.bytes;
	for (i = 0; i < backend->nr_segments; i++) {
		struct zram_backend_segment *segment = &backend->segments[i];

		stats->live_bytes += segment->live_bytes;
		stats->invalid_bytes += segment->invalid_bytes;
		stats->padding_bytes += segment->padding_bytes;
		stats->failed_bytes += segment->failed_bytes;
		stats->records += segment->records;
		if (segment->state == ZRAM_BACKEND_SEGMENT_OPEN)
			stats->open_tail_bytes += backend->segment_bytes - segment->append_tail;
		if (segment->state == ZRAM_BACKEND_SEGMENT_FREE)
			stats->free_segments++;
	}
	mutex_unlock(&backend->lock);
	return 0;
}

void zram_backend_destroy(struct zram *zram)
{
	struct zram_backend *backend = zram->backend;
	struct zram_backend_record *record, *next;

	if (!backend)
		return;
	/*
	 * init_lock write excludes every API caller. I/O is synchronous; there
	 * are no workers or exported pointers to wait for in this implementation.
	 * This must change before adding asynchronous slot adapters or GC.
	 */
	zram->backend = NULL;
	list_for_each_entry_safe(record, next, &backend->records, node) {
		struct zram_backend_segment *segment;

		segment = &backend->segments[record->info.location.segment_id];
		unpublish_record(segment, record);
		kfree(record);
	}
	if (backend->reserved_pages)
		bitmap_clear(zram->bitmap, backend->reserved_start, backend->reserved_pages);
	buffer_free(&backend->write_buffer);
	kvfree(backend->segments);
	mutex_destroy(&backend->lock);
	kfree(backend);
}

#ifdef CONFIG_ZRAM_SEGMENT_BACKEND_KUNIT_TEST
/*
 * Slow test-only consistency audit. No pointers escape the mutex; this is not
 * a GC iterator or production lookup interface.
 */
int zram_backend_test_validate_indexes(struct zram *zram)
{
	struct zram_backend *backend = zram->backend;
	struct zram_backend_record *record;
	u64 global_count = 0, segment_count = 0;
	unsigned int i;
	int ret = -EINVAL;

	if (!backend)
		return -ENODEV;
	mutex_lock(&backend->lock);
	for (i = 0; i < backend->nr_segments; i++) {
		struct zram_backend_segment *segment = &backend->segments[i];
		u64 count = 0, live = 0;

		list_for_each_entry(record, &segment->record_list, segment_node) {
			struct zram_backend_location *loc = &record->info.location;

			if (loc->segment_id != i ||
			    loc->segment_generation != segment->generation ||
			    !same_owner(&record->info.owner, &segment->owner) ||
			    !loc->payload_len || loc->offset > segment->append_tail ||
			    loc->payload_len > segment->append_tail - loc->offset ||
			    find_record(backend, record->id) != record)
				goto out;
			count++;
			live += loc->payload_len;
		}
		if (count != segment->records || live != segment->live_bytes ||
		    segment->append_tail > backend->segment_bytes ||
		    segment->append_tail != live + segment->invalid_bytes +
					    segment->padding_bytes + segment->failed_bytes)
			goto out;
		if (segment->state == ZRAM_BACKEND_SEGMENT_FREE &&
		    (count || segment->append_tail || segment->owner.id))
			goto out;
		segment_count += count;
	}
	list_for_each_entry(record, &backend->records, node)
		global_count++;
	if (global_count != segment_count)
		goto out;
	ret = 0;
out:
	mutex_unlock(&backend->lock);
	return ret;
}

/* Snapshot IDs in segment-list order. Capacity errors leave output untouched. */
int zram_backend_test_segment_records(struct zram *zram, u64 segment_id,
				      u64 *ids, unsigned int capacity)
{
	struct zram_backend *backend = zram->backend;
	struct zram_backend_record *record;
	struct zram_backend_segment *segment;
	u64 count = 0;
	unsigned int i = 0;
	int ret;

	if (!backend)
		return -ENODEV;
	if (!ids && capacity)
		return -EINVAL;
	mutex_lock(&backend->lock);
	if (segment_id >= backend->nr_segments) {
		ret = -EINVAL;
		goto out;
	}
	segment = &backend->segments[segment_id];
	list_for_each_entry(record, &segment->record_list, segment_node)
		count++;
	if (count > INT_MAX || count > capacity) {
		ret = -ENOSPC;
		goto out;
	}
	list_for_each_entry(record, &segment->record_list, segment_node)
		ids[i++] = record->id;
	ret = i;
out:
	mutex_unlock(&backend->lock);
	return ret;
}

int zram_backend_test_configure(struct zram *zram, u64 segment_bytes,
				unsigned int nr_segments, unsigned int block_size,
				zram_backend_test_io_fn submit, void *private)
{
	struct zram_backend *backend = zram->backend;
	int ret;

	if (!backend || !submit || (nr_segments && segment_bytes > U64_MAX / nr_segments))
		return -EINVAL;
	mutex_lock(&backend->lock);
	ret = setup_segments(backend, 0, segment_bytes, nr_segments, block_size);
	if (!ret) {
		backend->submit = submit;
		backend->io_private = private;
	}
	mutex_unlock(&backend->lock);
	return ret;
}
#endif
