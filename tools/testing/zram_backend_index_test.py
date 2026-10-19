#!/usr/bin/env python3
"""Exercise production index helpers with a small userspace list shim.

Not a kernel build: does not validate kernel locks, allocator/BIO, real write
call sites or reset concurrency. KUnit covers call-site integration when built.
"""
import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / "google-modules/soc/gs/drivers/block/zram"


def extract(source, marker):
    start = source.index(marker)
    brace = source.index("{", start)
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if not depth:
                return source[start:pos + 1]
    raise ValueError("unterminated declaration")


PREAMBLE = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint64_t u64;
typedef uint32_t u32;
struct list_head { struct list_head *next, *prev; };
static unsigned int warnings;
#define WARN_ON_ONCE(c) ((c) ? (++warnings, true) : false)
#define container_of(p, type, field) ((type *)((char *)(p) - offsetof(type, field)))
static void INIT_LIST_HEAD(struct list_head *head)
{
    head->next = head->prev = head;
}
static bool list_empty(const struct list_head *head)
{
    return head->next == head;
}
static void list_add_tail(struct list_head *node, struct list_head *head)
{
    node->next = head;
    node->prev = head->prev;
    head->prev->next = node;
    head->prev = node;
}
static void list_del(struct list_head *node)
{
    assert(node->next && node->prev);
    assert(node->next->prev == node && node->prev->next == node);
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->next = node->prev = NULL;
}
struct zram_backend { struct list_head records; };
"""

TEST = r"""
#define RECORDS 64
#define SEGMENTS 3
static struct zram_backend backend;
static struct zram_backend_segment segments[SEGMENTS];
static struct zram_backend_record records[RECORDS];
static bool active[RECORDS];
static uint32_t seed = 0x538e2U;

static unsigned int random_value(void)
{
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

static void audit(void)
{
    unsigned int global_seen[RECORDS] = {0}, segment_seen[RECORDS] = {0};
    unsigned int global_count = 0, count;
    struct list_head *node;
    for (node = backend.records.next; node != &backend.records; node = node->next) {
        struct zram_backend_record *r = container_of(node, struct zram_backend_record, node);
        size_t at = (size_t)(r - records);
        assert(at < RECORDS && active[at]);
        assert(++global_count <= RECORDS);
        assert(node->next->prev == node && node->prev->next == node);
        assert(++global_seen[at] == 1);
    }
    for (unsigned int s = 0; s < SEGMENTS; ++s) {
        struct zram_backend_segment *seg = &segments[s];
        u64 live = 0;
        count = 0;
        for (node = seg->record_list.next; node != &seg->record_list; node = node->next) {
            struct zram_backend_record *r =
                container_of(node, struct zram_backend_record, segment_node);
            size_t at = (size_t)(r - records);
            assert(at < RECORDS && active[at]);
            assert(++count <= RECORDS);
            assert(node->next->prev == node && node->prev->next == node);
            assert(++segment_seen[at] == 1);
            assert(r->info.location.segment_id == s);
            assert(r->info.location.segment_generation == seg->generation);
            assert(r->info.owner.id == seg->owner.id);
            assert(r->info.owner.generation == seg->owner.generation);
            live += r->info.location.payload_len;
        }
        assert(count == seg->records && live == seg->live_bytes);
        assert(seg->append_tail == live + seg->invalid_bytes);
        if (!count) {
            assert(list_empty(&seg->record_list));
            assert(seg->state == ZRAM_BACKEND_SEGMENT_FREE);
            assert(seg->owner.id == 0 && seg->append_tail == 0);
        }
    }
    for (unsigned int i = 0; i < RECORDS; ++i)
        assert(global_seen[i] == (unsigned int)active[i] &&
               segment_seen[i] == (unsigned int)active[i]);
}

static void add(unsigned int at, unsigned int s)
{
    struct zram_backend_segment *seg = &segments[s];
    struct zram_backend_record *r = &records[at];
    assert(!active[at]);
    if (seg->state == ZRAM_BACKEND_SEGMENT_FREE) {
        ++seg->generation;
        seg->owner.id = s + 1;
        seg->owner.generation = seg->generation;
        seg->state = ZRAM_BACKEND_SEGMENT_OPEN;
    }
    r->id = at + 1;
    r->info.owner = seg->owner;
    r->info.location.segment_id = s;
    r->info.location.segment_generation = seg->generation;
    r->info.location.payload_len = 1 + random_value() % 4096;
    r->info.location.offset = seg->append_tail;
    seg->append_tail += r->info.location.payload_len;
    publish_record(&backend, seg, r);
    active[at] = true;
}

static void remove_record(unsigned int at)
{
    struct zram_backend_record *r = &records[at];
    struct zram_backend_segment *seg = &segments[r->info.location.segment_id];
    unpublish_record(seg, r);
    active[at] = false;
    recycle_empty_segment(seg);
}

int main(void)
{
    INIT_LIST_HEAD(&backend.records);
    for (unsigned int s = 0; s < SEGMENTS; ++s)
        INIT_LIST_HEAD(&segments[s].record_list);
    audit();
    add(0, 0);
    add(1, 0);
    add(2, 0);
    remove_record(1); /* middle */
    audit();
    remove_record(0); /* head */
    remove_record(2); /* last, recycle */
    audit();
    add(3, 0); /* same segment, new generation */
    audit();
    /* A corrupt zero count must not turn a nonempty segment into FREE. */
    segments[0].records = 0;
    recycle_empty_segment(&segments[0]);
    assert(warnings == 1 && segments[0].state != ZRAM_BACKEND_SEGMENT_FREE);
    segments[0].records = 1;
    remove_record(3);
    audit();
    for (unsigned int step = 0; step < 20000; ++step) {
        unsigned int at = random_value() % RECORDS;
        if (active[at])
            remove_record(at);
        else
            add(at, random_value() % SEGMENTS);
        audit();
    }
    for (unsigned int at = 0; at < RECORDS; ++at)
        if (active[at])
            remove_record(at);
    audit();
    assert(warnings == 1);
    puts("PASS: production publish/unpublish/recycle helpers, middle/head/last "
         "removal, segment reuse, recycle guard and 20000 mixed operations");
    return 0;
}
"""


def main():
    source = (DRIVER / "zram_backend.c").read_text()
    header = (DRIVER / "zram_backend.h").read_text()
    owner = (DRIVER / "zram_owner.h").read_text()
    declarations = [
        (owner, "struct zram_backend_owner {"),
        (header, "enum zram_backend_format {"),
        (header, "struct zram_backend_location {"),
        (header, "struct zram_backend_record_info {"),
        (source, "enum zram_backend_segment_state {"),
        (source, "struct zram_backend_segment {"),
        (source, "struct zram_backend_record {"),
    ]
    code = [PREAMBLE]
    code += [extract(s, marker) + ";" for s, marker in declarations]
    code += [extract(source, marker) for marker in (
        "static void publish_record(",
        "static void unpublish_record(",
        "static void recycle_empty_segment(",
    )]
    code.append(TEST)
    with tempfile.TemporaryDirectory(prefix="zram-index-") as directory:
        c_file = Path(directory) / "index.c"
        executable = Path(directory) / "index"
        c_file.write_text("\n".join(code))
        subprocess.run([
            os.environ.get("CC", "gcc"), "-std=gnu11", "-Wall", "-Wextra",
            "-Werror", "-O1", "-fsanitize=undefined",
            str(c_file), "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True)
    print("Scope: production metadata helpers with userspace list shim; not a kernel/KUnit run.")


if __name__ == "__main__":
    main()
