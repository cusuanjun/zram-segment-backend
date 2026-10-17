/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ZRAM_BACKEND_H_
#define _ZRAM_BACKEND_H_

#include <linux/types.h>

/* Owner identity supplied by callers; zero id means unknown. */
struct zram_backend_owner {
	u64 id;
	u64 generation;
};

struct zram;

/* Input-window / buffer policy cap, not device geometry. */
#define ZRAM_BACKEND_MAX_BATCH_RECORDS 64

enum zram_backend_format {
	ZRAM_BACKEND_COMPRESSED,
	ZRAM_BACKEND_RAW,
};

/* Memory metadata only: all offsets/lengths are bytes, not an on-disk ABI. */
struct zram_backend_location {
	u64 segment_id;
	u64 segment_generation;
	u64 offset;
	u32 payload_len;
	/* No per-record header/padding yet; batch padding is counted separately. */
	u32 stored_len;
};

struct zram_backend_input {
	struct zram_backend_owner owner;
	const void *data;
	u32 len;
	enum zram_backend_format format;
};

struct zram_backend_record_info {
	struct zram_backend_owner owner;
	struct zram_backend_location location;
	enum zram_backend_format format;
};

struct zram_backend_stats {
	u64 live_bytes;
	u64 invalid_bytes;
	u64 padding_bytes;
	u64 failed_bytes;
	u64 free_segments;
	u64 records;
	u64 open_tail_bytes;
	/* Payload-page capacity only; excludes pointer array/metadata/input copies. */
	u64 write_buffer_bytes;
};

#ifdef CONFIG_ZRAM_SEGMENT_BACKEND
/*
 * init/configure/destroy require init_lock held for write.
 * Other operations require init_lock held for read for their entire duration.
 * All operations may sleep. Never call them with a zRAM slot spinlock held.
 * There is no production caller of configure/write/read/release yet.
 */
int zram_backend_init(struct zram *zram);
void zram_backend_destroy(struct zram *zram);

/*
 * Borrow the existing backing device; reserve an unused range in its legacy
 * bitmap. start, bytes and segment_bytes must be PAGE_SIZE aligned.
 * The device logical block size must be a power of two in [512, PAGE_SIZE].
 * Reserves one reusable write buffer of at most 64 pages, further capped by
 * segment size and BIO_MAX_VECS. Allocation failure leaves the backend
 * unconfigured. No guessed segment size or sysfs activation is provided.
 */
int zram_backend_configure(struct zram *zram, u64 start, u64 bytes,
			   u64 segment_bytes);

/*
 * Inputs are immutable independent snapshots, never live zs_map_object maps.
 * One batch contains only one owner and fits wholly in one segment AND the
 * configured reusable write buffer; otherwise returns E2BIG.
 * For a valid nr_records, output IDs are all zero on failure. On success,
 * each ID owns one record.
 * All bytes are written before IDs are returned. This does NOT commit a
 * zRAM slot or promise power-loss persistence. Caller retains the old slot
 * until its own conditional commit and releases IDs for rejected records.
 * IDs are never reused within a configured backend; reset invalidates all IDs.
 */
int zram_backend_write_batch(struct zram *zram,
			     const struct zram_backend_input *inputs,
			     unsigned int nr_records, u64 *record_ids);

/*
 * Bounded window (1..64 records) of immutable snapshots; may mix owners.
 * Group by complete owner (id + generation), then split on segment tail and
 * write-buffer capacity, without splitting an individual record across segments.
 * No long-lived queue, timer, page capture or zRAM slot commit is performed.
 *
 * Unlike write_batch, this operation can PARTIALLY SUCCEED. IDs match ORIGINAL
 * input positions, which need not form a prefix after grouping. On any return,
 * each nonzero ID owns a successfully stored record that caller must commit or
 * release. Zero IDs were not published: retain their original zRAM data.
 * On first preparation/I/O/space error, stop; do not roll back earlier batches.
 * Invalid input is detected across the whole window before the first I/O.
 */
int zram_backend_write_records(struct zram *zram,
			       const struct zram_backend_input *inputs,
			       unsigned int nr_records, u64 *record_ids);

/* Reads the stored bytes, not decompressed data. Serialized with release. */
int zram_backend_read(struct zram *zram, u64 record_id, void *dst,
		      size_t capacity, struct zram_backend_record_info *info);

/*
 * May sleep: not yet suitable for direct use by zram_free_page().
 * A future slot adapter needs reference pinning/deferred release.
 */
int zram_backend_release(struct zram *zram, u64 record_id);
int zram_backend_get_stats(struct zram *zram, struct zram_backend_stats *stats);
#else
static inline int zram_backend_init(struct zram *zram)
{
	return 0;
}

static inline void zram_backend_destroy(struct zram *zram)
{
}
#endif

#ifdef CONFIG_ZRAM_SEGMENT_BACKEND_KUNIT_TEST
struct page;
typedef int (*zram_backend_test_io_fn)(void *private, u64 offset,
				      struct page **pages, unsigned int nr_pages,
				      unsigned int bytes, bool write);
/* Test-only memory transport: never reserves or touches a real device. */
int zram_backend_test_configure(struct zram *zram, u64 segment_bytes,
				unsigned int nr_segments, unsigned int block_size,
				zram_backend_test_io_fn submit, void *private);
#endif

#endif /* _ZRAM_BACKEND_H_ */
