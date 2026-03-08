# Consolidated Adversarial Review (v7)

Six rounds of review (v1–v6) produced findings, corrections, and
corrections of corrections.  This document consolidates what we know,
what we got wrong, and — critically — what remains genuinely unsettled
because we lack empirical evidence.

---

## Settled: findings where analysis is complete

### 1. Per-write disk reservation is net-zero (confirmed in code)

`bch2_sum_sector_overwrites` (write.c:185-187) computes the net
`disk_sectors_delta`.  For any swap overwrite (pre-fragmented or not,
compressed or not): the new extent covers the same logical range as
the old → `disk_sectors_delta` = 0.  The reservation gate in
`bch2_extent_update` (write.c:327-332) is never reached.
`BCH_WRITE_check_enospc` is never evaluated.

**Status**: code-verified.  Skipping `check_enospc` for swap is
harmless belt-and-suspenders.

### 2. The two allocation gates are independent (confirmed in code)

- **Reservation gate** (`bch2_disk_reservation_add`): checks
  `c->sectors_available`.  Never reached for swap (see above).
- **Physical gate** (`bch2_bucket_alloc_trans` →
  `__dev_buckets_free`): searches for completely free buckets.
  Does NOT check `sectors_available`.

The disk reservation protects swap **indirectly**: it reduces
`sectors_available` → normal writers ENOSPC sooner → stop consuming
free buckets → free buckets remain for swap's physical allocator.

**Status**: code-verified.

### 3. Interior btree nodes are not pinned (confirmed in code)

`bch2_swap_pin_btree_range` calls `__for_each_btree_node` with
depth=0, which iterates leaf nodes only.  Interior nodes are traversed
but not yielded to the callback.  The writeup (line 614-620) now
correctly states this and explains the `bc->freeable` fallback.

**Status**: code-verified.  Not a bug — interior nodes are few, hot,
and covered by the pre-reserve.  The writeup accurately describes the
trade-off.

### 4. Inode btree uses key cache (confirmed in code)

Both inode lookups in the write path use `BTREE_ITER_cached`
(write.c:227, write.c:1383).  In steady state, inode reads come from
the in-memory key cache hash table, not from btree nodes.

The 3 MB per-write estimate (3 btrees × 4 levels × 256 KB) is a
worst-case that includes the first inode access.  Practical steady-
state is closer to 2 btrees (extents + alloc).  The 16 MB pre-reserve
is conservatively sized either way.

**Status**: code-verified.  The writeup's 3 MB figure is a safe upper
bound.

### 5. PF_MEMALLOC on reads is correct (confirmed by reasoning)

Swap reads (page faults) set PF_MEMALLOC.  This prevents read-path
allocations from entering reclaim, which could trigger swap writes
competing for the same btree locks → deadlock.  The code comment
(swap.c:328-338) now explains this.

**Status**: reasoning-verified.  Correct for deadlock avoidance.

### 6. Prefragmentation comment corrected

The old comment claimed pre-existing blocks "serve as space
reservation."  This was wrong for COW (COW allocates new blocks;
old blocks aren't free space).  The comment now correctly states
pre-frag does NOT prevent ENOSPC; the disk reservation does.

**Status**: fixed in code (swap.c:138-145).

### 7. Error path cleanup

`bch2_swap_activate` now has a cleanup path if `add_swap_extent` fails:
releases btree cache reserve, disk reservation, and unpins nodes
(swap.c:288-300).

**Status**: fixed in code.

### 8. BUG() guarded for production

The 10-second stall BUG is now `BUG_ON(IS_ENABLED(CONFIG_BCACHEFS_DEBUG))`
(swap.c:379).  Only fires in debug builds.

**Status**: fixed in code.

### 9. Configurable prefragmentation granularity

`bcachefs.swap_prefrag_kb=` cmdline parameter (default 4, minimum 4)
allows larger fragments (e.g. 64 KB) to reduce btree metadata on
small-RAM systems (swap.c:58-74).

**Status**: fixed in code.

---

## Settled with caveats: reasoning is sound but untested

### 10. Dead-key accumulation and headroom

With 1:1 COW replacements, net key count is constant but physical
node occupancy drifts: old keys in written bsets become dead, new keys
append to the dirty bset.  Worst case: 50% dead + 50% live = 100%.

Journal reclaim compacts nodes (merges bsets), resetting dead-key
accumulation.  The writeup's feedback loop argument (heavy swap →
journal fills → reclaim writes nodes → dead keys compacted) is sound
in theory.

**Caveat**: "pre-splitting gives ~1/3 headroom" is ambiguous.  With
50% pre-split fill, overwriting all keys reaches exactly 100% — zero
margin, not 1/3.  The 1/3 refers to the split-threshold headroom
(100% - 66%), which is a different quantity and applies to the
*unsplit* case.

**Needs empirical data**: instrument node fill levels during sustained
swap pressure to verify journal reclaim keeps up.  The gap between
"math works in steady state" and "transient bursts can exceed the
bound" is real but unmeasured.

### 11. `bc->freeable` shrinker protection

Pre-allocated btree node buffers go on `bc->freeable`.  Bumping
`nr_reserve` reduces the shrinker's scan budget indirectly (via
`count` callback → `btree_cache_can_free(live[0])`).

- In small-memory scenarios (128-256M): `live[0]` is small,
  `nr_reserve` bump makes `can_free` near zero → effective.
- In large-memory scenarios: `live[0].nr >> nr_reserve` → shrinker
  budget remains large → pre-allocated buffers can be drained.
- Hardcoded "skip first 3" (cache.c:597) protects 3 nodes (768 KB)
  unconditionally.  `list_add` adds at head; allocator iterates from
  head → the 3 protected nodes are the first ones used.

**Needs empirical data**: monitor `bc->nr_freeable` during swap
pressure to see if pre-allocated buffers survive the shrinker in
practice.  The indirect protection via `nr_reserve` is hard to reason
about in isolation — it depends on `live[0]` size, shrinker
frequency, and allocation rate.

---

## Unsettled: requires empirical evidence

### 12. Reservation sizing: `swap_size` vs smaller

The reservation of `swap_size` sectors is conservative.  It serves
two purposes:

**(a) Ensure free buckets for COW allocation**: the physical allocator
needs completely free buckets.  Swap's open write points need ~1-4
buckets (4-16 MB) at a time.  The reservation ensures normal writers
don't consume all free space.

**(b) Keep copygc healthy**: on a fragmented filesystem, free sectors
may be distributed across partially-used buckets with zero completely-
free buckets.  Copygc consolidates these but needs free space and I/O
bandwidth.  A larger reservation gives copygc more room.

The reservation is much larger than needed for (a) alone.  Whether
(b) justifies the full `swap_size` depends on real-world fragmentation
patterns.

**Prior oscillation**: v4 called this "High" (wrong — conflated
reservation and physical gates).  v5 said "32 KB" (wrong — that's the
reservation gate which is never reached).  v6 corrected to focus on
the physical gate and acknowledged the dual purpose.

**Needs empirical data**:
- On a 90%+ full filesystem with active non-swap writers, does swap
  with a `swap_size` reservation work reliably?
- Same test with a 32 MB reservation — does it also work, or does
  copygc struggle?
- Measure free-bucket count over time under sustained swap + normal I/O
  with various reservation sizes.

### 13. Free-bucket exhaustion on fragmented filesystems

Even with `actual_free >= swap_size`, all free space could be in
partially-used buckets → zero completely-free buckets → physical
allocation fails.  Copygc must consolidate, but competes for I/O.

The writeup says "ENOSPC during reclaim is structurally impossible"
(line 1397) in one place and "extremely unlikely ... but not
structurally impossible" (line 1317-1320) in another.  The latter
is more accurate.

**Needs empirical data**:
- Create a heavily fragmented filesystem (many small files, then
  delete half) → measure free-bucket count vs `actual_free`.
- Activate swap on this filesystem and run sustained swap pressure.
- Does copygc keep up?  If not, at what fragmentation level does it
  fail?

### 14. Prefragmentation granularity trade-off

At 4 KB: 65K extents for 256 MB swap → 64 MB btree metadata →
catastrophic on 128M VMs (5-16% pass rate).

At 64 KB: 4K extents → 4 MB btree metadata.  But the first swap
write to each 64 KB chunk does a 1→3 extent split under memory
pressure (adding 2 keys to the btree).

**Needs empirical data**:
- Test 64 KB granularity at 128M/192M RAM.
- Measure: pass rate, btree cache pressure, node fill levels.
- Does the deferred 1→3 split under pressure cause issues, or is
  it absorbed by the headroom?

---

## Retracted findings

| Finding | Version | Why retracted |
|---------|---------|---------------|
| Disk reservation blocks swap writes via check_enospc | v4 (O) | `disk_sectors_delta` = 0 for overwrites; gate never reached |
| "32 KB transient need" for reservation | v5 | Conflated reservation gate (never reached) with physical gate |
| Reservation sizing "orders of magnitude too large" | v5 | Over-reservation has legitimate dual purpose (copygc health) |

---

## Summary of open findings

| # | Finding | Severity | Evidence needed |
|---|---------|----------|-----------------|
| 10 | Dead-key accumulation may exceed journal reclaim rate | Medium | Instrument node fill under sustained swap |
| 11 | `bc->freeable` shrinker protection is indirect | Medium | Monitor `nr_freeable` during swap pressure |
| 12 | Reservation size may over-starve or under-protect | Medium | Test swap + normal I/O at various reservation sizes on near-full FS |
| 13 | Free-bucket exhaustion on fragmented FS | Medium | Test swap on a pre-fragmented filesystem |
| 14 | 64 KB prefrag granularity untested | Medium | Run ablation matrix at 64 KB |

| # | Finding | Severity | Status |
|---|---------|----------|--------|
| 1 | Per-write disk reservation is net-zero | — | Code-verified, no action needed |
| 2 | Two allocation gates independent | — | Code-verified |
| 3 | Interior nodes not pinned | Low | Correctly documented; pre-reserve covers |
| 4 | Inode btree uses key cache | — | 3 MB is safe upper bound |
| 5 | PF_MEMALLOC on reads | — | Correct |
| 6–9 | Various code fixes | — | All applied |

---

## New findings from code review (this round)

### 15. Per-write reservation fallback via `check_allocated` (not a bug — undocumented)

The DIO write path (direct.c:511-514) always attempts a per-write
`bch2_disk_reservation_get`.  For swap on a near-full FS this fails
(because `sectors_available` is reduced by the swap_activate
reservation).

The code then falls through to `bch2_dio_write_check_allocated(dio)`
(direct.c:264-273), which verifies the logical range already has
physical extents.  For a pre-fragmented swap file, this always passes
→ the write proceeds with `op.res.sectors = 0`.  Later in
`bch2_extent_update`, `disk_sectors_delta = 0` → no further
reservation needed.

This is a **correct and important** part of the ENOSPC story.  The
writeup's ENOSPC section (lines 1273-1297) doesn't mention it.  The
actual three-layer protection is:

1. Swap_activate reservation → starves normal writers → preserves
   free buckets
2. Per-write reservation fails → `check_allocated` fallback succeeds
   (swap file has extents) → write proceeds without reservation
3. `disk_sectors_delta = 0` → reservation gate in `bch2_extent_update`
   is never reached

The writeup should document layer 2 — it's the mechanism that
actually lets swap writes proceed when `sectors_available` is zero.

### 16. Prefragmentation minimum doesn't account for PAGE_SIZE > 4 KB (bug)

swap.c:180:
```c
unsigned frag_sectors = max_t(unsigned, bch2_swap_prefrag_kb, 4) * 2;
```

The minimum is hardcoded to 4 KB (8 sectors).  On architectures with
larger pages (ARM64: PAGE_SIZE = 64 KB), fragments would be smaller
than a page.  A single swap write (one page = 128 sectors) would span
16 fragments → 16 extent keys modified per write → defeats the 1:1
replacement goal.

**Fix**: clamp to PAGE_SIZE, not 4 KB:
```c
unsigned frag_sectors = max_t(unsigned, bch2_swap_prefrag_kb * 2,
                              PAGE_SECTORS);
```

And update the cmdline minimum:
```c
if (kstrtouint(s, 10, &val) == 0 && val >= (PAGE_SIZE >> 10))
```

On x86_64 (PAGE_SIZE = 4 KB): no change.  On ARM64 (PAGE_SIZE =
64 KB): minimum becomes 64 KB instead of 4 KB.

### 17. Disk reservation failure leaks pinned btree nodes (bug)

swap.c:284-288: if `bch2_disk_reservation_get` fails, the function
returns without unpinning the btree nodes that were pinned at line
259.  The `add_swap_extent` error path (lines 313-324) has the
cleanup, but the disk reservation error path does not.

```c
if (disk_ret) {
    bch_err(c, "...");
    return disk_ret;   // ← pinned nodes NOT unpinned
}
```

**Fix**: add cleanup before the return:
```c
if (disk_ret) {
    bch_err(c, "...");
    if (bch2_swap_pin_enabled)
        bch2_swap_pin_unpin_nodes(c, inode, false);
    return disk_ret;
}
```

### 18. Comment says "Warn after 2 s, BUG after 10 s" (line 88) — comment matches code, but writeup line 1127 says "BUG after 10 s" without noting the CONFIG_BCACHEFS_DEBUG guard (trivial)

The code at line 424 is `BUG_ON(IS_ENABLED(CONFIG_BCACHEFS_DEBUG))`.
The comment at line 88 should mention the debug guard to avoid
confusing readers.  Similarly, the writeup's "Crash-on-Hang Policy"
section (line 1127) says "BUG() after 10 seconds" without mentioning
the debug guard.

### Summary update

| # | Finding | Severity |
|---|---------|----------|
| 15 | `check_allocated` fallback undocumented | Low (works correctly; writeup gap) |
| 16 | Prefrag minimum ignores PAGE_SIZE > 4 KB | **Medium** (portability bug on ARM64) |
| 17 | Disk reservation failure leaks pinned nodes | **Low** (resource leak on a rare error path) |
| 18 | Comment/writeup don't mention debug guard on BUG | Trivial |
