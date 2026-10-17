// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/percpu.h>
#include <linux/slab.h>

#include "zram_drv.h"
#include "zram_owner.h"

static int owner_test_init(struct kunit *test)
{
	struct zram *zram;

	zram = kunit_kzalloc(test, sizeof(*zram), GFP_KERNEL);
	if (!zram)
		return -ENOMEM;
	zram->table = kunit_kzalloc(test, sizeof(*zram->table), GFP_KERNEL);
	if (!zram->table)
		return -ENOMEM;
	zram->pcp_stats = alloc_percpu(struct zram_stats);
	if (!zram->pcp_stats)
		return -ENOMEM;
	if (zram_owner_init(zram)) {
		free_percpu(zram->pcp_stats);
		return -ENOMEM;
	}
	test->priv = zram;
	return 0;
}

static void owner_test_exit(struct kunit *test)
{
	struct zram *zram = test->priv;

	zram_owner_destroy(zram);
	free_percpu(zram->pcp_stats);
}

static void uid_grouping_and_incarnation(struct kunit *test)
{
	struct zram *zram = test->priv;
	struct zram_backend_owner a, b, c;

	KUNIT_ASSERT_EQ(test, zram_owner_bind(zram, 501, 10123, 1), 0);
	KUNIT_ASSERT_EQ(test, zram_owner_bind(zram, 502, 10123, 1), 0);
	KUNIT_ASSERT_EQ(test, zram_owner_bind(zram, 601, 10124, 1), 0);
	a = zram_owner_lookup(zram, 501);
	b = zram_owner_lookup(zram, 502);
	c = zram_owner_lookup(zram, 601);
	KUNIT_EXPECT_EQ(test, a.id, b.id);
	KUNIT_EXPECT_EQ(test, a.generation, b.generation);
	KUNIT_EXPECT_NE(test, a.id, c.id);
	/* Same Android appId in a different Android user must stay separate. */
	KUNIT_ASSERT_EQ(test, zram_owner_bind(zram, 602, 110123, 1), 0);
	c = zram_owner_lookup(zram, 602);
	KUNIT_EXPECT_NE(test, a.id, c.id);
	KUNIT_ASSERT_EQ(test, zram_owner_bind(zram, 701, 10123, 2), 0);
	c = zram_owner_lookup(zram, 701);
	KUNIT_EXPECT_EQ(test, a.id, c.id);
	KUNIT_EXPECT_NE(test, a.generation, c.generation);
}

static void immutable_binding_and_unknown(struct kunit *test)
{
	struct zram *zram = test->priv;
	struct zram_backend_owner owner, snapshot;

	KUNIT_EXPECT_EQ(test, zram_owner_bind(zram, 0, 10123, 1), -EINVAL);
	KUNIT_EXPECT_EQ(test, zram_owner_bind(zram, 1, 10123, 0), -EINVAL);
	KUNIT_EXPECT_EQ(test, zram_owner_bind(zram, 1, (u32)-1, 1), -EINVAL);
	KUNIT_ASSERT_EQ(test, zram_owner_bind(zram, 501, 10123, 1), 0);
	KUNIT_EXPECT_EQ(test, zram_owner_bind(zram, 501, 10123, 1), 0);
	KUNIT_EXPECT_EQ(test, zram_owner_bind(zram, 501, 10124, 1), -EEXIST);
	KUNIT_EXPECT_EQ(test, zram_owner_bind(zram, 501, 10123, 2), -EEXIST);
	snapshot = zram_owner_lookup(zram, 501);
	KUNIT_ASSERT_EQ(test, zram_owner_unbind(zram, 501), 0);
	owner = zram_owner_lookup(zram, 501);
	KUNIT_EXPECT_EQ(test, owner.id, (u64)0);
	KUNIT_EXPECT_EQ(test, snapshot.id, (u64)10124);
	KUNIT_EXPECT_EQ(test, zram_owner_unbind(zram, 501), -ENOENT);
	/* Unknown is not conflated with the valid full UID zero. */
	KUNIT_ASSERT_EQ(test, zram_owner_bind(zram, 800, 0, 1), 0);
	owner = zram_owner_lookup(zram, 800);
	KUNIT_EXPECT_EQ(test, owner.id, (u64)1);
}

static u64 test_provider(struct page *page, void *private)
{
	unsigned int *calls = private;

	(*calls)++;
	return 501;
}

static void reject_unattributed_pages(struct kunit *test)
{
	struct zram *zram = test->priv;
	struct zram_backend_owner owner;
	unsigned int calls = 0;
	struct page *page = alloc_page(GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, page);
	owner = zram_owner_capture(zram, page);
	KUNIT_EXPECT_EQ(test, owner.id, (u64)0);
	KUNIT_EXPECT_EQ(test, zram_owner_set_provider(zram, test_provider, &calls), 0);
	KUNIT_EXPECT_EQ(test, zram_owner_set_provider(zram, test_provider, &calls), -EBUSY);
	owner = zram_owner_capture(zram, page);
	KUNIT_EXPECT_EQ(test, owner.id, (u64)0);
	KUNIT_EXPECT_EQ(test, calls, 0U);
	KUNIT_EXPECT_EQ(test, zram_owner_set_provider(zram, NULL, NULL), 0);
	__free_page(page);
}

static void slot_owner_follows_content(struct kunit *test)
{
	struct zram *zram = test->priv;
	struct zram_backend_owner owner, unknown = {};

	KUNIT_ASSERT_EQ(test, zram_owner_bind(zram, 501, 10123, 1), 0);
	owner = zram_owner_lookup(zram, 501);
	/* SAME content exercises the real slot replacement/free path, no pool. */
	zram_slot_update(zram, 0, 1, 0, &owner);
	KUNIT_EXPECT_EQ(test, zram->table[0].owner.id, owner.id);
	KUNIT_ASSERT_EQ(test, zram_owner_unbind(zram, 501), 0);
	KUNIT_EXPECT_EQ(test, zram->table[0].owner.id, owner.id);
	owner.generation = 2;
	zram_slot_update(zram, 0, 2, 0, &owner);
	KUNIT_EXPECT_EQ(test, zram->table[0].owner.generation, (u64)2);
	zram_slot_update(zram, 0, 3, 0, &unknown);
	KUNIT_EXPECT_EQ(test, zram->table[0].owner.id, (u64)0);
	KUNIT_EXPECT_EQ(test, zram->table[0].owner.generation, (u64)0);
}

static struct kunit_case owner_cases[] = {
	KUNIT_CASE(uid_grouping_and_incarnation),
	KUNIT_CASE(immutable_binding_and_unknown),
	KUNIT_CASE(reject_unattributed_pages),
	KUNIT_CASE(slot_owner_follows_content),
	{}
};

static struct kunit_suite owner_suite = {
	.name = "zram-owner",
	.init = owner_test_init,
	.exit = owner_test_exit,
	.test_cases = owner_cases,
};
kunit_test_suite(owner_suite);
MODULE_LICENSE("GPL");
