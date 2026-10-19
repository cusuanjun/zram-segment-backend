#!/usr/bin/env python3
"""Run actual backend C and KUnit case bodies with a userspace shim.

Not a kernel/KUnit run: excludes real BIO/configure, emulates memory/locks and
KUnit assertions. Also injects late errors into multi-buffer GC transfers.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import sys
sys.dont_write_bytecode = True
from zram_backend_index_test import PREAMBLE, extract

ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / "google-modules/soc/gs/drivers/block/zram"

SHIM = r"""
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
typedef uint8_t u8;
#define CONFIG_ZRAM_SEGMENT_BACKEND 1
#define CONFIG_ZRAM_SEGMENT_BACKEND_KUNIT_TEST 1
#define PAGE_SIZE HOST_PAGE_SIZE
#define BIO_MAX_VECS 256
#define GFP_NOIO 0
#define GFP_KERNEL 0
#define __GFP_ZERO 0
#define U64_MAX UINT64_MAX
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define min_t(t,a,b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define DIV_ROUND_UP(a,b) (((a)+(b)-1)/(b))
#define round_up(a,b) (((a)+(b)-1) & ~((__typeof__(a))((b)-1)))
#define round_down(a,b) ((a) & ~((__typeof__(a))((b)-1)))
#define IS_ALIGNED(a,b) (!((a) & ((b)-1)))
#define is_power_of_2(a) ((a) && !((a)&((a)-1)))
struct mutex { int held; };
static void mutex_init(struct mutex *m) { m->held = 0; }
static void mutex_lock(struct mutex *m) { assert(!m->held); m->held = 1; }
static void mutex_unlock(struct mutex *m) { assert(m->held); m->held = 0; }
static void mutex_destroy(struct mutex *m) { assert(!m->held); }
struct rw_semaphore { int held; };
#define init_rwsem(m) ((m)->held = 0)
#define down_write(m) ((void)(m))
#define up_write(m) ((void)(m))
#define down_read(m) ((void)(m))
#define up_read(m) ((void)(m))
struct page { unsigned char data[PAGE_SIZE]; };
static int fail_page_alloc;
static struct page *alloc_page(int flags) {
    (void)flags;
    if (fail_page_alloc) { --fail_page_alloc; return NULL; }
    return calloc(1, sizeof(struct page));
}
#define __free_page(p) free(p)
#define page_address(p) ((p)->data)
#define kzalloc(n,f) calloc(1,n)
#define kcalloc(n,s,f) calloc(n,s)
#define kvcalloc(n,s,f) calloc(n,s)
#define kfree(p) free(p)
#define kvfree(p) free(p)
#define bitmap_clear(p,a,b) ((void)(p),(void)(a),(void)(b))
#define list_entry(p,t,m) container_of(p,t,m)
#define list_for_each_entry(p,h,m) \
 for (p=list_entry((h)->next,__typeof__(*p),m); &p->m!=(h); \
      p=list_entry(p->m.next,__typeof__(*p),m))
#define list_for_each_entry_safe(p,n,h,m) \
 for (p=list_entry((h)->next,__typeof__(*p),m), \
      n=list_entry(p->m.next,__typeof__(*p),m); &p->m!=(h); \
      p=n,n=list_entry(n->m.next,__typeof__(*n),m))
struct zram { struct zram_backend *backend; void *bitmap; struct rw_semaphore init_lock; };
struct kunit { void *priv; void *allocs[2048]; unsigned int nr; };
static void *test_alloc(struct kunit *t, size_t n) {
    void *p = calloc(1,n); assert(t->nr < ARRAY_SIZE(t->allocs));
    t->allocs[t->nr++] = p; return p;
}
#define kunit_kmalloc(t,n,f) test_alloc(t,n)
#define kunit_kzalloc(t,n,f) test_alloc(t,n)
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"failure line %d: %s\n",__LINE__,#c); abort(); } } while(0)
#define KUNIT_ASSERT_EQ(t,a,b) CHECK((a)==(b))
#define KUNIT_EXPECT_EQ(t,a,b) CHECK((a)==(b))
#define KUNIT_EXPECT_NE(t,a,b) CHECK((a)!=(b))
#define KUNIT_ASSERT_NOT_NULL(t,a) CHECK((a)!=NULL)
#define KUNIT_EXPECT_FALSE(t,a) CHECK(!(a))
struct kunit_case { void (*run)(struct kunit *); const char *name; };
#define KUNIT_CASE(f) { f, #f }
struct kunit_suite { const char *name; int (*init)(struct kunit *); void (*exit)(struct kunit *); struct kunit_case *test_cases; };
#define kunit_test_suite(s)
#define MODULE_LICENSE(s)
"""

EXTRA = r"""
/* Restrict the existing buffer to one page to exercise streaming boundaries.
 * Segment stays four pages; this models a segment larger than the I/O cap.
 */
static void stream_failure_cases(void)
{
    for (unsigned int failure = 0; failure < 3; failure++) {
        struct kunit t = {0};
        CHECK(backend_test_init(&t) == 0);
        struct backend_fixture *f = t.priv;
        struct zram_backend *b = f->zram.backend;
        u8 *data = test_alloc(&t, PAGE_SIZE), *out = test_alloc(&t, PAGE_SIZE);
        for (unsigned int i=0; i<PAGE_SIZE; i++) data[i]=(u8)(i*23);
        struct zram_backend_input in[4] = {0};
        u64 ids[4];
        for (unsigned int i=0; i<4; i++) {
            in[i].owner.id=1; in[i].data=data; in[i].len=PAGE_SIZE/2+1;
        }
        CHECK(zram_backend_write_batch(&f->zram,in,4,ids)==0);
        CHECK(zram_backend_release(&f->zram,ids[3])==0);
        struct zram_backend_record_info old[3], now;
        for (unsigned int i=0; i<3; i++)
            CHECK(zram_backend_read(&f->zram,ids[i],out,PAGE_SIZE,&old[i])==0);
        b->write_buffer.bytes=PAGE_SIZE;
        if (failure==0) fail_page_alloc=1;
        if (failure==1) f->fail_write_at=f->writes+2;
        if (failure==2) f->fail_read_at=f->reads+3;
        int ret=zram_backend_gc_segment(&f->zram,old[0].location.segment_id,
                                        old[0].location.segment_generation);
        CHECK(ret==(failure==0 ? -ENOMEM : -EIO));
        for (unsigned int i=0; i<3; i++) {
            CHECK(zram_backend_read(&f->zram,ids[i],out,PAGE_SIZE,&now)==0);
            CHECK(now.location.segment_id==old[i].location.segment_id);
            CHECK(now.location.offset==old[i].location.offset);
            CHECK(!memcmp(data,out,in[i].len));
        }
        CHECK(zram_backend_test_validate_indexes(&f->zram)==0);
        CHECK(zram_backend_gc_segment(&f->zram,old[0].location.segment_id,
                                      old[0].location.segment_generation)==0);
        for (unsigned int i=0; i<3; i++) {
            CHECK(zram_backend_read(&f->zram,ids[i],out,PAGE_SIZE,&now)==0);
            CHECK(now.location.segment_id!=old[i].location.segment_id);
            CHECK(now.location.offset==(u64)i*in[i].len);
            CHECK(!memcmp(data,out,in[i].len));
        }
        backend_test_exit(&t);
        for (unsigned int i=0;i<t.nr;i++) free(t.allocs[i]);
    }
}

static void mixed_gc_lifecycle(void)
{
    struct kunit t={0};
    CHECK(backend_test_init(&t)==0);
    struct backend_fixture *f=t.priv;
    u8 *data=test_alloc(&t,PAGE_SIZE), *out=test_alloc(&t,PAGE_SIZE);
    u64 ids[32]={0};
    unsigned int lengths[32]={0}, owners[32]={0};
    uint32_t seed=0x175abe;
    for (unsigned int i=0;i<PAGE_SIZE;i++) data[i]=(u8)(i*31);
    for (unsigned int step=0;step<600;step++) {
        seed=seed*1664525U+1013904223U;
        unsigned int at=(seed>>16)%32;
        if (ids[at]) {
            if (seed&1) {
                CHECK(zram_backend_release(&f->zram,ids[at])==0);
                ids[at]=0;
            } else {
                struct zram_backend_record_info info;
                CHECK(zram_backend_read(&f->zram,ids[at],out,PAGE_SIZE,&info)==0);
                int ret=zram_backend_gc_segment(&f->zram,info.location.segment_id,
                                                info.location.segment_generation);
                CHECK(ret==0 || ret==-EALREADY);
            }
        } else {
            lengths[at]=1+(seed%PAGE_SIZE);
            owners[at]=1+(seed%3);
            struct zram_backend_input in={
                .owner={owners[at],7},.data=data,.len=lengths[at]};
            int ret=zram_backend_write_batch(&f->zram,&in,1,&ids[at]);
            CHECK(ret==0 || ret==-ENOSPC);
        }
        CHECK(zram_backend_test_validate_indexes(&f->zram)==0);
        for (unsigned int i=0;i<32;i++) if(ids[i]) {
            struct zram_backend_record_info info;
            CHECK(zram_backend_read(&f->zram,ids[i],out,PAGE_SIZE,&info)==0);
            CHECK(info.owner.id==owners[i] && info.owner.generation==7);
            CHECK(info.location.payload_len==lengths[i]);
            CHECK(!memcmp(data,out,lengths[i]));
        }
    }
    backend_test_exit(&t);
    for(unsigned int i=0;i<t.nr;i++) free(t.allocs[i]);
}

static void exhausted_generation_and_geometry(void)
{
    struct kunit t={0};
    CHECK(backend_test_init(&t)==0);
    struct backend_fixture *f=t.priv;
    struct zram_backend *b=f->zram.backend;
    u8 value=42;
    struct zram_backend_input in={.owner={1,1},.data=&value,.len=1};
    u64 id, id2;
    CHECK(zram_backend_write_batch(&f->zram,&in,1,&id)==0);
    CHECK(zram_backend_write_batch(&f->zram,&in,1,&id2)==0);
    for(unsigned int i=1;i<TEST_SEGMENTS;i++) b->segments[i].generation=U64_MAX;
    CHECK(zram_backend_gc_segment(&f->zram,0,1)==-ENOSPC);
    in.owner.id=2;
    CHECK(zram_backend_write_batch(&f->zram,&in,1,&id2)==-ENOSPC);
    backend_test_exit(&t);
    for(unsigned int i=0;i<t.nr;i++) free(t.allocs[i]);
    struct zram_backend empty={0};
    CHECK(setup_segments(&empty,0,PAGE_SIZE,1,512)==-EINVAL);
    CHECK(empty.segments==NULL);
}

int main(void)
{
    unsigned int count=0;
    for (struct kunit_case *c=backend_test_suite.test_cases; c->run; ++c) {
        struct kunit t={0};
        CHECK(backend_test_suite.init(&t)==0);
        c->run(&t);
        backend_test_suite.exit(&t);
        for(unsigned int i=0;i<t.nr;i++) free(t.allocs[i]);
        count++;
    }
    stream_failure_cases();
    mixed_gc_lifecycle();
    exhausted_generation_and_geometry();
    CHECK(warnings==0);
    printf("PASS: %u backend case bodies + streaming failures/retry + 600 mixed steps + geometry/generation, PAGE_SIZE=%u\n",
           count, (unsigned int)PAGE_SIZE);
    return 0;
}
"""


def strip_includes(text):
    return "\n".join(line for line in text.splitlines() if not line.startswith("#include"))


def main():
    source = (DRIVER / "zram_backend.c").read_text()
    for marker in ("static int block_io(", "int zram_backend_configure("):
        source = source.replace(extract(source, marker), "")
    preamble = PREAMBLE.replace("struct zram_backend { struct list_head records; };", "")
    code = "\n".join((preamble, SHIM,
        extract((DRIVER / "zram_owner.h").read_text(), "struct zram_backend_owner {") + ";",
        strip_includes((DRIVER / "zram_backend.h").read_text()),
        strip_includes(source),
        strip_includes((DRIVER / "zram_backend_test.c").read_text()), EXTRA))
    with tempfile.TemporaryDirectory(prefix="zram-gc-") as directory:
        c_file = Path(directory) / "backend.c"
        c_file.write_text(code)
        for page in (4096, 16384, 65536):
            exe = Path(directory) / f"backend-{page}"
            subprocess.run([os.environ.get("CC", "gcc"), "-std=gnu11", "-Wall", "-Wextra",
                            "-Werror", "-O1", "-fsanitize=undefined", f"-DHOST_PAGE_SIZE={page}",
                            str(c_file), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
    print("Scope: userspace transport/allocator/lock shim; not a kernel build or KUnit run.")


if __name__ == "__main__":
    main()
