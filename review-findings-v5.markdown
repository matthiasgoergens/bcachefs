# Adversarial Review v5: Self-Review of Finding O

Finding O (v4) claimed the disk reservation at swap_activate blocks swap
writes from succeeding because `BCH_WRITE_check_enospc` causes per-write
reservation checks against the reduced `sectors_available` counter.

**The analysis was wrong.**  Here's the correction and what survives.

## The check_enospc flag is never evaluated for swap COW writes

`bch2_extent_update` (write.c:322-332) computes the **net**
`disk_sectors_delta` via `bch2_sum_sector_overwrites`:

```c
*disk_sectors_delta += sectors * bch2_bkey_nr_ptrs_allocated(new);
*disk_sectors_delta -= sectors * bch2_bkey_nr_ptrs_fully_allocated(old);
```

For a 1:1 COW swap write (pre-fragmented, same size, same replica
count): new_sectors = old_sectors, same pointer count →
`disk_sectors_delta` = **0**.

The reservation check:
```c
if (disk_res &&
    disk_sectors_delta > (s64) disk_res->sectors)
        try(bch2_disk_reservation_add(c, disk_res, ...,
                !check_enospc || !usage_increasing
                ? BCH_DISK_RESERVATION_NOFAIL : 0));
```

With `disk_sectors_delta` = 0 and `disk_res->sectors` ≥ 0, the
condition is **false**.  `bch2_disk_reservation_add` is never called.
The `check_enospc` flag is never evaluated.  The scenario from finding
O (swap writes blocked by reduced `sectors_available`) cannot occur.

This holds even WITHOUT pre-fragmentation: a COW write to the middle
of a large extent still has net `disk_sectors_delta` = 0 (the new page
replaces the same sector range in the old extent).  The only case where
`disk_sectors_delta` > 0 is writing past EOF, which swap never does.

## The applied fix is harmless defence-in-depth

The code change (skip `BCH_WRITE_check_enospc` for swap files in
direct.c:503-504) makes no functional difference for 1:1 replacements.
It's correct as a guard against hypothetical edge cases (replica count
change mid-flight, non-pre-fragmented writes with metadata overhead)
but it was solving a non-existent problem for the normal path.

## What survives: reservation sizing

The swap_activate reservation of `swap_size` sectors DOES work for its
intended purpose: it reduces `sectors_available`, preventing normal
writers from consuming all free space, ensuring the foreground allocator
can find free buckets for COW swap blocks.

But the size is massively over-reserved:

- Each COW write holds 1 new block temporarily (the old block is freed
  atomically in the same transaction commit).
- With mempool=8 concurrent writes: transient peak = 8 × PAGE_SECTORS
  = **32 KB**.
- The reservation is `swap_size` = potentially **GBs**.

On a near-full filesystem (100 GB, 96 GB used, 4 GB swap):
- `actual_free` = 4 GB
- swap reservation = 4 GB
- `sectors_available` = 0
- ALL normal writes get ENOSPC — even though swap only ever needs
  32 KB of transient headroom at a time

A right-sized reservation: `mempool_size × PAGE_SECTORS × nr_replicas
× safety_factor` ≈ 256 KB–1 MB.  This covers the transient COW peak
with generous margin without starving normal writers.

Counter-argument for the large reservation: it also prevents the
filesystem from filling up to the point where copygc, journal reclaim,
and other background operations struggle.  The over-reservation acts
as a "minimum free space" guarantee while swap is active.  Whether
this is a feature or a bug depends on the use case — a 4% disk
capacity hold on a 100 GB filesystem is fine; on a 20 GB filesystem
with a 4 GB swap file it's 20%, which is aggressive.

## Remaining findings from v4 (unchanged)

| Finding | Severity | Status |
|---------|----------|--------|
| ~~check_enospc blocks swap~~ (finding O) | ~~High~~ **Retracted** | check_enospc never evaluated for net-0 overwrites |
| Reservation size starves normal writers | Medium | `swap_size` is orders of magnitude larger than the ~32 KB transient need |
| Interior btree nodes not pinned (finding Q) | Medium | Code iterates depth 0 (leaves only); writeup corrected to note this |
| `bch2_swap_inflight` inaccurate for async (finding S) | Trivial | Counter decremented at submission, not I/O completion |
