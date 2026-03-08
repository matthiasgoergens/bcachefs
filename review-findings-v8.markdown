# Review v8: Test Regression After Code Changes

## Regression observed

The earlier ablation run (kernel #36, ablation-20260301-155454) was
15/16 PASS.  The latest run (ablation-20260301-201536), after the
review-driven code changes, shows 10/22 HUNG — including
configurations that previously passed consistently:

| Configuration | Earlier (155454) | Latest (201536) |
|---------------|-----------------|-----------------|
| large-128m-all | PASS | **HUNG** |
| large-128m-thrash | PASS | **HUNG** |
| large-128m-drop | PASS | **HUNG** |
| large-192m-all | PASS | **HUNG** |
| large-128m-thrash-nopin | PASS | **HUNG** |
| small-128m-nopin | PASS | **HUNG** |
| small-192m-nopin | PASS | **HUNG** |

The `small-*-all` configurations (small FS, few btree nodes) still
pass.  The failures concentrate in `large-*` (4G FS, 10K files, many
btree nodes, 65K prefrag keys).

## What the logs show

`large-192m-all.log`:
```
swap activated on inode ... (65535 pages, 65536 fragments @ 4 KB,
  4 nodes pinned, 64/64 btree nodes pre-reserved)
faulted 82 MB (21060 pages)
faulted 163 MB (41796 pages)
swap: extents leaf at 80% fill (26205/32748 u64s)
swap: extents leaf at 80% fill (26212/32748 u64s)
[timeout kill — no further progress]
```

Key observations:
- **swap_free = 262140 kB** (full 256 MB) — zero pages were swapped.
  The system hung before completing even one swap write.
- **Nodes at 80% after prefragmentation** — 65K extent keys at 4 KB
  granularity pack leaf nodes to ~80% before any swap I/O begins.
- The 80% fill warnings are new instrumentation added in the review
  fixes; the fill level itself was always there.

## Likely causes of the regression

Code changes between the passing and failing runs:

1. **Disk reservation** (`bch2_disk_reservation_get` for `swap_size`):
   on a 4G filesystem with 10K files and a 256 MB swap file, this
   reserves 256 MB of `sectors_available`.  If the filesystem was
   already near capacity (from the 10K test files), this could push
   `sectors_available` near zero — starving journal reclaim of disk
   reservation capacity for btree node writeback.  Journal reclaim
   writes dirty btree nodes as COW; if it can't get a disk reservation
   for the new node copy, it stalls → journal fills → swap writes
   block on journal space → hang.

   **This needs verification**: check whether journal reclaim's btree
   node writes go through `bch2_disk_reservation_add` and whether they
   use `BCH_DISK_RESERVATION_NOFAIL` or not.

2. **16 MB btree cache pre-reserve** (`bch2_btree_cache_add_reserve`):
   allocates 64 × 256 KB = 16 MB of btree node buffers at swapon.
   On a 192 MB VM with ~128 MB free after boot, this consumes 12.5%
   of free RAM.  Combined with the 65K-extent btree (which needs
   cache space for its leaf nodes), this may push the system past the
   tipping point.

3. **4 KB prefragmentation filling nodes to 80%**: this was always
   the case, but the other changes reduced the margin.  With nodes
   at 80% and the first round of swap writes adding dead keys, nodes
   hit 100% before journal reclaim can compact them.

## Most likely root cause

The combination of all three: prefrag fills nodes to 80%, the disk
reservation reduces headroom for journal reclaim, and the 16 MB
pre-reserve consumes RAM that was previously available as btree cache
or emergency reserves.  Each change is individually reasonable; together
they tip the large-FS 128-192M configurations past the edge.

## Immediate next step

Test with `bcachefs.swap_prefrag_kb=64` (or disable prefrag entirely)
to reduce btree metadata from 65K keys (~80% node fill) to 4K keys
(~5% node fill).  This is the highest-leverage change: 16× fewer
keys, dramatically less cache pressure, and nodes start with plenty
of headroom for dead-key accumulation.

If that fixes the `large-*` configs, it confirms finding #10/#14
from v7: 4 KB granularity is the root cause, not the disk reservation
or pre-reserve.  If it doesn't fix them, the disk reservation
interaction with journal reclaim needs investigation.

## Bisect result: root cause found

The regression was NOT the disk reservation or btree cache pre-reserve
(kernel #38, same code as #35 + those features, passes 2/2).

Bisect:

| Kernel | Changes | large-192m-all |
|--------|---------|----------------|
| #38 | none (baseline) | PASS 2/2 |
| #39 | only commit.c (80% fill monitoring) | PASS 2/2 |
| #40 | only write.c prealloc (GFP_KERNEL) | **HUNG 2/2** |
| #41 | only write.c prealloc (GFP_NOWAIT) | PASS 2/2 |

**Root cause**: `kmalloc(2048, GFP_KERNEL)` in the write index
kworker (`bch2_write_point_do_index_updates`) entering direct reclaim.
The kworker runs during swap writeback; entering reclaim from there
amplifies the journal deadlock — reclaim tries to swap more pages,
which needs journal space, which is what we're trying to free.

Same class of bug as the original deadlock: a `GFP_KERNEL` allocation
in the swap write path entering reclaim.  The `BCH_WRITE_swap` /
`PF_MEMALLOC` mechanism prevents this for the main write thread, but
the pre-allocation was added in the kworker path where `PF_MEMALLOC`
hadn't been set yet (or was set after the allocation).

**Fix**: `GFP_KERNEL` → `GFP_NOWAIT`.  Under `PF_MEMALLOC` (set by
the `BCH_WRITE_swap` flag handler), `GFP_NOWAIT` uses emergency
reserves without entering reclaim.  If the allocation fails, it fails
fast instead of deadlocking.

Fixed in kernel #41, carried forward to the final kernel #44 and the
draft PR.

### Lesson

The v8 "likely causes" section was wrong — it blamed the disk
reservation and btree cache pre-reserve based on reasoning rather
than measurement.  The actual root cause was a single GFP flag in
a code path I didn't examine closely enough.  This is exactly the
pattern identified in v7 finding #10 ("needs empirical data") — pure
reasoning about memory pressure interactions is unreliable without
controlled experiments.
