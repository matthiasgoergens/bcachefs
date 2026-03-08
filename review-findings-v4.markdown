# Adversarial Review v4: Code vs Writeup

Concentrated on verifying the code matches the writeup's claims.

## O. Disk reservation doesn't actually protect swap writes (High)

The writeup (line 1264-1279) says: "`bch2_swap_activate` now calls
`bch2_disk_reservation_get` ... This earmarks the entire swap file's
worth of disk space.  Other writers see the reserved space as
already-used; they cannot take it."

The reservation works by decrementing `c->sectors_available` (the
global free-space counter minus all active reservations).  This
correctly prevents normal writers from consuming the reserved space.

**But swap writes are also blocked by the reduced counter.**

In `vfs/direct.c:503`:
```c
if (IS_SWAPFILE(&inode->v))
    dio->op.flags |= BCH_WRITE_swap;
dio->op.flags |= BCH_WRITE_check_enospc;   // ← unconditional
```

`BCH_WRITE_check_enospc` is set for ALL DIO writes, including swap.
Each swap COW write takes its own per-write disk reservation from the
same `c->sectors_available` pool that the swap_activate reservation
already reduced.

Scenario where this fails:
1. Filesystem: 200 sectors free
2. swap_activate reserves 100 → `sectors_available` = 100
3. Normal writers consume 100 → `sectors_available` = 0
4. Swap COW write needs 1 sector per-write reservation → **ENOSPC**

The swap_activate reservation prevented normal writers from taking all
200, but it didn't create a dedicated pool for swap.  Both swap and
normal writes compete for the remaining `sectors_available`.

**Fix**: skip `BCH_WRITE_check_enospc` for swap files.  The
swap_activate reservation guarantees physical free blocks exist (the
counter went down, but the blocks are on disk).  The foreground
allocator will find them.

```c
if (IS_SWAPFILE(&inode->v))
    dio->op.flags |= BCH_WRITE_swap;
else
    dio->op.flags |= BCH_WRITE_check_enospc;
```

Alternatively, use `BCH_DISK_RESERVATION_NOFAIL` in the write op's
reservation to let it draw from the pre-reserved pool.

## P. Prefragmentation comment claims space reservation it doesn't provide (Medium)

`vfs/swap.c:138-139`:
```c
 * Also serves as space reservation: every page has a physical block
 * allocated, so swap writes can't fail with ENOSPC.
```

This is wrong for COW writes.  A COW write allocates a **new** block
and frees the **old** block after the btree commit.  The old block is
not free space — it's in use by the current version of the extent.
Pre-allocated blocks do not serve as free-space reservation.

On a 100% full filesystem (swap file + other files fill all blocks),
a COW write needs 1 free block to write the new copy.  There are 0
free blocks.  The pre-allocated blocks are "used", not "free".

The disk reservation added at lines 243-266 is the actual fix.  The
comment at line 138-139 should be removed or corrected to say
"pre-fragmentation ensures 1:1 key replacement, but the disk
reservation (below) is what prevents ENOSPC."

## Q. Interior btree nodes are not pinned (Medium)

The writeup (line 614-617) says: "Also pinned: the alloc btree nodes
(for block allocation) and interior extent btree nodes.  Interior nodes
are few (~2-3 for even a large swap file).  These are pinned eagerly at
swapon — negligible cost."

The code (`bch2_swap_pin_btree_range`, line 75-96) calls
`__for_each_btree_node` with `_depth = 0`, which iterates **leaf
nodes only** (depth 0 = leaves).  Interior nodes are traversed during
the iteration but are NOT yielded to the callback, so
`set_btree_node_noevict(b)` only applies to leaves.

This means:
- Extent btree interior nodes: **not pinned**
- Inode btree interior nodes: **not pinned**
- Alloc btree: pinned POS_MIN to SPOS_MAX at depth 0 = **leaves only**

In practice, interior nodes are few and hot (always in cache). But
under extreme memory pressure, the shrinker could evict an interior
node, and then a swap write's btree traversal would need to read it
back — requiring a page allocation under PF_MEMALLOC.

**Fix**: either iterate at each depth level to pin interior nodes too,
or accept that interior nodes are covered by the `bc->freeable`
pre-reserve (which is what the btree cache reserve is for).  The
writeup should be corrected to not claim interior nodes are pinned.

## R. Error path leaks disk reservation (Low)

In `bch2_swap_activate`, if `add_swap_extent` (line 288) fails, the
function returns the error.  But the disk reservation (taken at line
257-265) and btree cache reserve (taken at line 279) are not cleaned up.

The disk reservation holds `swap_sectors` of `sectors_available` until
the filesystem is unmounted.  The btree cache reserve holds `nr_reserve`
incremented.

Similarly, if `bch2_btree_cache_add_reserve` partially succeeds (say 40
of 64 requested nodes), and later `add_swap_extent` fails, the 40 pre-
allocated nodes and the `nr_reserve` bump are leaked.

**Fix**: add a cleanup path that calls `bch2_disk_reservation_put` and
`bch2_btree_cache_remove_reserve` on error.

## S. `bch2_swap_inflight` counter is inaccurate for async writes (Trivial)

`atomic_dec(&bch2_swap_inflight)` at line 345 runs after
`bch2_write_iter` returns.  For async writes (`-EIOCBQUEUED`), the
write is still in progress — the bio was submitted but hasn't completed.
The counter is decremented at submission, not completion.

Similarly, `elapsed_ns` (line 347) measures submission latency, not
I/O completion latency.  The WARN/BUG checks (lines 369-380) only
fire if submission takes >2s/10s, not if the actual I/O stalls.

For testing purposes this is fine (the stalls we care about are in the
submission path, not in disk I/O).  But the counter name `inflight` is
misleading — it's really `in_submission`.

## T. Stall detection BUG() at 10s is aggressive for production (Low)

`SWAP_IO_BUG_NS = 10s` (line 72).  Under heavy disk I/O (spinning
disks, I/O scheduler contention, virtio latency with 50 VMs), a
legitimate swap I/O could take >10s.  The BUG() would crash a system
that would have recovered.

The comment block at line 62 says "we want to crash early with a useful
stack trace" — correct for debug/test kernels, but this code is not
behind `#ifdef CONFIG_BCACHEFS_DEBUG` or a runtime toggle.  It will
fire in production.

**Fix**: either guard with `CONFIG_BCACHEFS_DEBUG`, or make it a
`WARN_ON_ONCE` instead of `BUG()`, or add a cmdline toggle
(`bcachefs.swap_bug_on_stall`).

---

## Self-review of finding O: the analysis was wrong

Finding O claimed that `BCH_WRITE_check_enospc` causes swap writes to
fail by taking per-write reservations from the global `sectors_available`
counter.  After tracing the actual code, **this analysis is incorrect.**

### What actually happens (write.c:322-332)

`bch2_extent_update` calls `bch2_sum_sector_overwrites` to compute the
**net** `disk_sectors_delta`:

```c
*disk_sectors_delta += sectors * bch2_bkey_nr_ptrs_allocated(new);
*disk_sectors_delta -= sectors * bch2_bkey_nr_ptrs_fully_allocated(old);
```

For a 1:1 COW swap write (pre-fragmented, same size, same replica
count): new_sectors = old_sectors, same ptr count → `disk_sectors_delta`
= **0**.

Then the reservation check (write.c:327-332):
```c
if (disk_res &&
    disk_sectors_delta > (s64) disk_res->sectors)
        try(bch2_disk_reservation_add(c, disk_res, ...,
                !check_enospc || !usage_increasing
                ? BCH_DISK_RESERVATION_NOFAIL : 0));
```

With `disk_sectors_delta` = 0 and `disk_res->sectors` ≥ 0, the
condition is **false**.  `bch2_disk_reservation_add` is never called.
The `check_enospc` flag is never evaluated.

**`BCH_WRITE_check_enospc` was always irrelevant for 1:1 COW swap
writes.**  Finding O's scenario (swap writes blocked by reduced
`sectors_available`) cannot occur because the code path that checks
`sectors_available` is never reached.

### The applied fix is harmless but unnecessary

The code change (skip `check_enospc` for swap files) is correct as
defence-in-depth for edge cases where `disk_sectors_delta` > 0 (e.g.,
non-pre-fragmented writes, replica count changes).  But for the normal
1:1 replacement case, it makes no difference.

### The real question: is the reservation SIZE correct?

The swap_activate reservation of `swap_size` sectors does work: it
reduces `sectors_available`, which prevents normal writers from
consuming all free space, which ensures the foreground allocator
(`bch2_alloc_sectors_start`) can find free buckets for new COW blocks.

But it's **massively over-reserved**.  Each COW write holds one new
block temporarily (freed atomically in the same transaction).  With
mempool=8 concurrent writes, the transient peak is 8 × page_sectors
= **32 KB**.  Reserving the entire swap file (potentially GBs)
starves normal writers of `sectors_available` unnecessarily.

On a near-full filesystem (e.g. 100GB, 96GB used, 4GB swap file):
- `actual_free` = 4GB
- swap reservation = 4GB
- `sectors_available` = 0
- ALL normal writes get ENOSPC — even though there's 4GB of
  physical free space, most of which swap will never use
  concurrently

A right-sized reservation would be: `mempool_size × page_sectors ×
nr_replicas × safety_factor` ≈ 256 KB–1 MB.  This covers the
transient COW peak with margin, without starving normal writers.

Alternatively: reserve `swap_size` but use a mechanism that lets swap
writes bypass the `sectors_available` check (which, as shown above,
already happens naturally for 1:1 replacements).  Normal writers
still see the reduced `sectors_available` and get ENOSPC sooner,
which is the protective effect we want.

### When the analysis IS correct: non-pre-fragmented writes

If pre-fragmentation is disabled or the swap file has large extents,
a COW write to the middle of a large extent creates 3 keys.  The
net `disk_sectors_delta` is still 0 (the new page replaces the same
range in the old extent), so the reservation check still doesn't fire.

The only case where `disk_sectors_delta` > 0 is a write that adds
NEW data (extending the file, writing past EOF).  Swap writes don't
do this — they always overwrite existing ranges.

### Updated severity

Finding O is downgraded from **High** to **Low** (the check_enospc
flag was never the problem).  The reservation sizing question is a
usability concern (starving normal writers on near-full filesystems),
not a correctness bug.

## Summary

| Finding | Severity | Category |
|---------|----------|----------|
| ~~Disk reservation blocks swap writes~~ (finding O) | **Low** (was: High) | check_enospc was never evaluated for net-0 overwrites; fix is harmless belt-and-suspenders |
| Reservation SIZE may starve normal writers on near-full FS | Medium | `swap_size` reservation is orders of magnitude larger than the ~32KB transient need |
| Prefragmentation comment claims false space reservation | ~~Medium~~ | Fixed by user |
| Interior btree nodes not pinned despite writeup claim | Medium | Code-writeup gap |
| Error path leaks disk reservation | ~~Low~~ | Fixed by user |
| `bch2_swap_inflight` inaccurate for async | Trivial | Misleading counter name |
| BUG() at 10s not guarded for production | ~~Low~~ | Fixed by user (CONFIG_BCACHEFS_DEBUG guard) |
