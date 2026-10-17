/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ZRAM_OWNER_H_
#define _ZRAM_OWNER_H_

#include <linux/types.h>

/* Zero is unknown. Identity is independent of process and segment lifetimes. */
struct zram_backend_owner {
	u64 id;
	u64 generation;
};

struct zram;
struct page;
struct zram_owner_registry;

/*
 * Provider returns a nonzero, lifetime-unique memory-cgroup cookie, NOT a
 * reusable bare memcg ID, pointer, PID, blkcg ID or the current task's UID.
 * Called under a read spinlock: must not sleep, allocate or reenter this API.
 * Must safely identify the original swap page's charge owner, or return zero.
 */
typedef u64 (*zram_owner_page_fn)(struct page *page, void *private);

#ifdef CONFIG_ZRAM_SEGMENT_BACKEND
int zram_owner_init(struct zram *zram);
/* Device I/O and compression must be drained before destroy. */
void zram_owner_destroy(struct zram *zram);
/*
 * Internal trusted registration API, not a userspace ABI.
 * uid is the complete Android UID in the initial user namespace.
 * app_generation is nonzero and shared by all memcgs of one app incarnation;
 * it must change before that UID is assigned to a different app incarnation.
 * Mapping is immutable until unbound. Unbind does not invalidate stored slots.
 * Device lifetime must be held stable across every API call.
 */
int zram_owner_bind(struct zram *zram, u64 memcg_cookie, u32 uid,
		    u64 app_generation);
int zram_owner_unbind(struct zram *zram, u64 memcg_cookie);
struct zram_backend_owner zram_owner_lookup(struct zram *zram, u64 memcg_cookie);
/*
 * NULL unregisters and waits for active provider calls. The provider must
 * keep its code and private data alive until unregister returns. Only one
 * provider may be installed. Caller serializes provider control operations.
 */
int zram_owner_set_provider(struct zram *zram, zram_owner_page_fn fn, void *private);
struct zram_backend_owner zram_owner_capture(struct zram *zram, struct page *page);
#else
static inline int zram_owner_init(struct zram *zram) { return 0; }
static inline void zram_owner_destroy(struct zram *zram) {}
static inline struct zram_backend_owner
zram_owner_capture(struct zram *zram, struct page *page)
{
	struct zram_backend_owner owner = {};
	return owner;
}
#endif
#endif
