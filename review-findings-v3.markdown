# Adversarial Review v3: ENOSPC and Disk Pressure

## L. COW swap writes can ENOSPC — and the kernel can't handle that

Each COW swap write allocates a **new** physical block before freeing
the old one.  There is no disk space reservation in the current code.

If the filesystem is near full:

1. `bch2_alloc_sectors_start` can't find a free bucket
2. The swap write returns an error from `bch2_dio_write_loop`
3. `bch2_swap_rw` returns the error to the swap subsystem
4. The kernel was trying to free memory by swapping — if the swap
   write fails, it can't free memory

The kernel's swap subsystem does not handle ENOSPC from `swap_rw`
gracefully.  `swap_writepage_fs` (mm/page_io.c) treats errors as I/O
failures and sets `PageError`.  Under sustained write failures the
system spirals: can't free memory, can't swap, OOM.

**This is arguably a correctness requirement, not an optimization.**
Without a space reservation, there is no guarantee a swap write can
complete.

### Fix: disk reservation at swapon time

At `swap_activate`, call `bch2_disk_reservation_get` for `swap_size`
worth of blocks.  This earmarks enough free space for every COW copy
the swap file might need.  Release at `swap_deactivate`.

Cost: the reserved space is unavailable to normal file operations while
swap is active, even if most swap pages haven't been written yet.  For
a 4 GB swap file on a 100 GB filesystem, that's 4% of disk capacity
locked up.  Acceptable — the alternative is ENOSPC during reclaim.

The reservation also needs to account for btree metadata growth if
pre-fragmentation is not used (each COW write adds ~2 extent keys →
new btree nodes → disk space for those nodes).  With pre-fragmentation,
the btree is at steady state and no additional metadata space is needed.

### What about reads?

Swap reads don't allocate disk space (they just look up the existing
extent and read from it).  ENOSPC only affects writes.  But under
memory pressure the system needs to write (swap out) before it can
read (swap in), so write failures block everything.

## M. Near-full disk amplifies all existing risks

Even with a disk reservation for the swap file itself, a near-full
filesystem makes everything worse:

- **Copygc runs aggressively**, competing for I/O bandwidth with swap
  and journal reclaim.  Copygc moves data to consolidate fragmented
  buckets, writing new blocks and freeing old ones — exactly the same
  resources swap needs.

- **Journal reclaim gets harder**: writing dirty btree nodes to disk
  (to free journal space) requires allocating new blocks for the node
  COW copies.  If disk is near full, this allocation can block or fail,
  stalling journal reclaim, which stalls swap writes waiting for
  journal space.  Potential circular dependency.

- **Open bucket exhaustion**: with few free buckets, the foreground
  allocator falls into the btree path more often (scanning
  `BTREE_ID_freespace` for scraps), increasing per-write latency and
  lock contention.

- **Alloc btree gets hotter**: more free-space scanning means more
  alloc btree traversals, more cache pressure, more risk of cache
  misses under PF_MEMALLOC.

### Mitigation: minimum free space check at swapon

At `swap_activate`, check that the filesystem has enough free space
for the disk reservation plus a comfortable margin (e.g., 2× swap
size, or a minimum percentage like 10% of total capacity).  Refuse
to activate swap if the disk is too full.  Better to fail loudly
at `swapon` time than to fail silently during reclaim.

## N. Nocow fallback under disk pressure is not viable at runtime

The idea of falling back to nocow when disk space is tight doesn't
work for a file that was created and pre-fragmented as COW:

- **Nocow requires fixed physical locations.**  COW extents are
  written to new locations on every write; the old physical blocks
  are freed.  There is no stable physical address to overwrite
  in-place.

- **Switching COW→nocow at runtime would require rewriting all
  extents** to pinned locations, which itself needs disk space and
  memory — exactly what we don't have.

- **The inode `nocow` flag is a creation-time decision**, not a
  runtime toggle.  Setting it on an existing file doesn't retroactively
  pin its extents.

What COULD work as a fallback:

- **A separate small nocow swap file** created at setup time alongside
  the main COW swap file.  Under extreme disk pressure, the kernel can
  route pages to the nocow file (fixed locations, no allocation needed).
  This is effectively the "raw swap safety net" the writeup already
  recommends — but framed as a disk-pressure fallback, not just a
  memory-pressure fallback.

- **zram** as the fallback (no disk needed at all).  Compressed swap
  in RAM.  Under disk pressure, the system swaps to zram; when disk
  pressure eases, pages can migrate back to the bcachefs swap file.
  The kernel already supports multiple swap devices with priorities.

## Summary

| Finding | Severity | Recommendation |
|---------|----------|----------------|
| No disk reservation → ENOSPC during reclaim | **High** | `bch2_disk_reservation_get(swap_size)` at swapon |
| Near-full disk amplifies all swap risks | Medium | Minimum free space check at swapon |
| Nocow fallback not viable at runtime | Low | Use separate nocow file or zram as fallback instead |
