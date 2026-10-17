#!/usr/bin/env python3
"""Run the production pure packing helper without kernel headers or device I/O.

This is NOT a kernel/KUnit build. Only same_owner() and plan_batch() are
extracted verbatim; kernel allocation, locking, BIO and lifecycle are untested.
Requires Python 3 and a host C compiler (CC, default gcc).
"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "google-modules/soc/gs/drivers/block/zram/zram_backend.c"


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
    raise ValueError("unterminated function")


PREAMBLE = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define round_up(x, y) (((x) + (y) - 1U) & ~((y) - 1U))
struct zram_backend_owner { uint64_t id, generation; };
struct zram_backend_input {
    struct zram_backend_owner owner;
    const void *data;
    uint32_t len;
    int format;
};
"""
TEST = r"""
static uint32_t seed = 0x529a107dU;
static uint32_t random_value(void)
{
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

static void examples(void)
{
    struct zram_backend_input a[4] = {
        { .owner = {1, 7}, .len = 1200 },
        { .owner = {2, 7}, .len = 900 },
        { .owner = {1, 7}, .len = 1800 },
        { .owner = {1, 7}, .len = 1600 },
    };
    unsigned int indices[64], count, payload, next;
    next = plan_batch(a, 4, 0, 4096, 4096, indices, &count, &payload);
    assert(next == 3 && count == 2 && payload == 3000);
    assert(indices[0] == 0 && indices[1] == 2);
    next = plan_batch(a, 4, next, 8192, 4096, indices, &count, &payload);
    assert(next == 4 && count == 1 && payload == 1600);
    a[2].owner.generation = 8;
    next = plan_batch(a, 4, 0, 8192, 4096, indices, &count, &payload);
    assert(next == 4 && count == 2 && indices[1] == 3);
}

int main(void)
{
    unsigned int indices[64], seen[64];
    unsigned int page_sizes[] = {4096, 16384, 65536};
    unsigned int geometries = 0;
    examples();
    for (unsigned int g = 0; g < 3; ++g) {
        unsigned int page = page_sizes[g];
        for (unsigned int block = 512; block <= page; block *= 2) {
            ++geometries;
            for (unsigned int trial = 0; trial < 2000; ++trial) {
                struct zram_backend_input a[64] = {0};
                unsigned int n = 1 + random_value() % 64;
                for (unsigned int k = 0; k < n; ++k) {
                    a[k].owner.id = 1 + random_value() % 5;
                    a[k].owner.generation = 1 + random_value() % 2;
                    a[k].len = 1 + random_value() % page;
                }
                memset(seen, 0, sizeof(seen));
                /* Walk one owner's records across successive bounded batches. */
                for (unsigned int owner = 0; owner < n; ++owner) {
                    if (seen[owner])
                        continue;
                    unsigned int start = owner;
                    while (start < n) {
                        unsigned int count, payload, sum = 0, matched = 0;
                        unsigned int limit = round_up(a[start].len, block)
                            + (random_value() % 9) * block;
                        unsigned int next = plan_batch(a, n, start, limit,
                                            block, indices, &count, &payload);
                        assert(count && count <= n && next > start && next <= n);
                        for (unsigned int k = 0; k < count; ++k) {
                            unsigned int at = indices[k];
                            assert(at >= start && at < next && at < n);
                            assert(!seen[at]);
                            assert(same_owner(&a[start].owner, &a[at].owner));
                            if (k)
                                assert(indices[k - 1] < at);
                            seen[at] = 1;
                            sum += a[at].len;
                        }
                        for (unsigned int k = start; k < next; ++k)
                            if (same_owner(&a[start].owner, &a[k].owner))
                                ++matched;
                        assert(matched == count && sum == payload);
                        assert(round_up(payload, block) <= limit);
                        if (next < n) {
                            assert(same_owner(&a[start].owner, &a[next].owner));
                            assert(round_up(payload + a[next].len, block) > limit);
                        }
                        start = next;
                    }
                }
                for (unsigned int k = 0; k < n; ++k)
                    assert(seen[k] == 1);
            }
        }
    }
    printf("PASS: targeted cases + %u windows, 4/16/64 KiB page examples, "
           "512 B..PAGE_SIZE alignment, owner generations and exact-once packing\n",
           geometries * 2000);
    return 0;
}
"""


def main():
    source = SOURCE.read_text()
    code = "\n".join([
        PREAMBLE,
        extract(source, "static bool same_owner("),
        extract(source, "static unsigned int plan_batch("),
        TEST,
    ])
    with tempfile.TemporaryDirectory(prefix="zram-plan-") as directory:
        c_file = Path(directory) / "plan.c"
        executable = Path(directory) / "plan"
        c_file.write_text(code)
        subprocess.run([
            os.environ.get("CC", "gcc"), "-std=gnu11", "-Wall", "-Wextra",
            "-Werror", "-O1", "-fsanitize=undefined",
            str(c_file), "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True)
    print("Scope: extracted production packing helpers only; no kernel or I/O test.")


if __name__ == "__main__":
    main()
