// SPDX-License-Identifier: GPL-2.0-only
/* Explicit app identity mapping. No guessed Android cgroup topology. */
#include <linux/errno.h>
#include <linux/hashtable.h>
#include <linux/mm.h>
#include <linux/rwlock.h>
#include <linux/slab.h>

#include "zram_drv.h"
#include "zram_owner.h"

struct zram_owner_binding {
	struct hlist_node node;
	u64 memcg_cookie;
	struct zram_backend_owner owner;
};

struct zram_owner_registry {
	DECLARE_HASHTABLE(bindings, 6);
	rwlock_t lock;
	zram_owner_page_fn page_owner;
	void *private;
};

static struct zram_owner_binding *
find_binding(struct zram_owner_registry *registry, u64 cookie)
{
	struct zram_owner_binding *binding;

	hash_for_each_possible(registry->bindings, binding, node, cookie)
		if (binding->memcg_cookie == cookie)
			return binding;
	return NULL;
}

int zram_owner_init(struct zram *zram)
{
	struct zram_owner_registry *registry;

	if (zram->owners)
		return -EBUSY;
	registry = kzalloc(sizeof(*registry), GFP_KERNEL);
	if (!registry)
		return -ENOMEM;
	hash_init(registry->bindings);
	rwlock_init(&registry->lock);
	zram->owners = registry;
	return 0;
}

void zram_owner_destroy(struct zram *zram)
{
	struct zram_owner_registry *registry = zram->owners;
	struct zram_owner_binding *binding;
	struct hlist_node *tmp;
	int bucket;

	if (!registry)
		return;
	/* Lifecycle owner has drained I/O, provider/control calls and cookies. */
	hash_for_each_safe(registry->bindings, bucket, tmp, binding, node) {
		hash_del(&binding->node);
		kfree(binding);
	}
	kfree(registry);
	zram->owners = NULL;
}

int zram_owner_bind(struct zram *zram, u64 memcg_cookie, u32 uid,
		    u64 app_generation)
{
	struct zram_owner_registry *registry = zram->owners;
	struct zram_owner_binding *binding, *old;
	unsigned long flags;
	int ret = 0;

	if (!registry)
		return -ENODEV;
	if (!memcg_cookie || !app_generation || uid == (u32)-1)
		return -EINVAL;
	binding = kmalloc(sizeof(*binding), GFP_KERNEL);
	if (!binding)
		return -ENOMEM;
	binding->memcg_cookie = memcg_cookie;
	/* UID zero remains representable; owner zero is reserved for unknown. */
	binding->owner.id = (u64)uid + 1;
	binding->owner.generation = app_generation;
	write_lock_irqsave(&registry->lock, flags);
	old = find_binding(registry, memcg_cookie);
	if (old) {
		if (old->owner.id != binding->owner.id ||
		    old->owner.generation != app_generation)
			ret = -EEXIST;
	} else {
		hash_add(registry->bindings, &binding->node, memcg_cookie);
		binding = NULL;
	}
	write_unlock_irqrestore(&registry->lock, flags);
	kfree(binding);
	return ret;
}

int zram_owner_unbind(struct zram *zram, u64 memcg_cookie)
{
	struct zram_owner_registry *registry = zram->owners;
	struct zram_owner_binding *binding;
	unsigned long flags;

	if (!registry)
		return -ENODEV;
	write_lock_irqsave(&registry->lock, flags);
	binding = find_binding(registry, memcg_cookie);
	if (binding)
		hash_del(&binding->node);
	write_unlock_irqrestore(&registry->lock, flags);
	if (!binding)
		return -ENOENT;
	kfree(binding);
	return 0;
}

struct zram_backend_owner zram_owner_lookup(struct zram *zram, u64 memcg_cookie)
{
	struct zram_owner_registry *registry = zram->owners;
	struct zram_backend_owner owner = {};
	struct zram_owner_binding *binding;
	unsigned long flags;

	if (!registry || !memcg_cookie)
		return owner;
	read_lock_irqsave(&registry->lock, flags);
	binding = find_binding(registry, memcg_cookie);
	if (binding)
		owner = binding->owner;
	read_unlock_irqrestore(&registry->lock, flags);
	return owner;
}

int zram_owner_set_provider(struct zram *zram, zram_owner_page_fn fn, void *private)
{
	struct zram_owner_registry *registry = zram->owners;
	unsigned long flags;
	int ret = 0;

	if (!registry)
		return -ENODEV;
	write_lock_irqsave(&registry->lock, flags);
	if (fn && registry->page_owner) {
		ret = -EBUSY;
	} else {
		registry->page_owner = fn;
		registry->private = fn ? private : NULL;
	}
	write_unlock_irqrestore(&registry->lock, flags);
	return ret;
}

struct zram_backend_owner zram_owner_capture(struct zram *zram, struct page *page)
{
	struct zram_owner_registry *registry = zram->owners;
	struct zram_backend_owner owner = {};
	struct zram_owner_binding *binding;
	unsigned long flags;
	u64 cookie;

	/*
	 * First implementation supports ordinary private anonymous swap pages.
	 * In particular, reject partial-write bounce pages and non-swap writes.
	 * PageAnon alone does not establish exclusive ownership; classification
	 * follows the provider's memory-charge attribution policy.
	 */
	if (!registry || !page || PageCompound(page) ||
	    !PageAnon(page) || !PageSwapCache(page))
		return owner;
	read_lock_irqsave(&registry->lock, flags);
	if (registry->page_owner) {
		cookie = registry->page_owner(page, registry->private);
		binding = cookie ? find_binding(registry, cookie) : NULL;
		if (binding)
			owner = binding->owner;
	}
	read_unlock_irqrestore(&registry->lock, flags);
	return owner;
}
