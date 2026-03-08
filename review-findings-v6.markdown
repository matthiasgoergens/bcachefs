# Adversarial Review v6: Correcting v5 and New Findings

## Self-review: v5's "32 KB transient need" was wrong

v5 claimed the reservation is "orders of magnitude larger than the
~32 KB transient need."  This conflated two independent mechanisms.

### The two allocation gates are independent

The disk reservation counter (`c->sectors_available`) and the physical
bucket allocator (`bch2_bucket_alloc_trans` → `__dev_buckets_free`) are
**separate gates that don't check each other**:

- **Reservation gate** (`bch2_disk_reservation_add` in
  `bch2_extent_update`): checked when `disk_sectors_delta > 0`.  For
  1:1 COW swap writes: `disk_sectors_delta` = 0, gate is **never
  reached**.

- **Physical gate** (`bch2_bucket_alloc_trans` →
  `__dev_buckets_free`): searches the freespace/alloc btree for a
  completely free bucket.  Does NOT check `sectors_available`.  This is
  where a swap COW write can actually fail.

### How the reservation actually protects swap

1. `swap_activate` takes a reservation of `swap_size` →
   `sectors_available` drops
2. Normal writers try `bch2_disk_reservation_add` → see reduced
   `sectors_available` → ENOSPC sooner → stop consuming free buckets
3. Swap writes have `disk_sectors_delta` = 0 → skip the reservation
   gate entirely → go straight to physical allocation
4. Physical allocation succeeds because normal writers were starved of
   reservations before they could consume the remaining free buckets

So the reservation protects swap **indirectly** by throttling normal
writers.  The "32 KB" figure from v5 was based on the reservation gate
(which swap never hits).  The real protection is on the physical gate
(which needs actual free buckets).

### Right-sizing the reservation

The physical-gate need is: enough completely-free buckets for the COW
allocator's open write points.  Typically ~1-4 open buckets at 4 MB
each = 4-16 MB.  Plus headroom for copygc and journal reclaim to
function = another few MB.

A reservation of ~32-64 MB would cover the physical need with margin.
`swap_size` (potentially GBs) is conservative overkill that also
serves as a "minimum free space" policy to keep the filesystem healthy
while swap is active.

Whether this dual purpose is intentional and desirable, or an
accidental over-reservation that starves normal writers, is a design
question — not a bug.

## U. Free-bucket exhaustion on fragmented filesystems (Medium)

This is the strongest remaining finding that NO prior review identified.

The physical bucket allocator needs **completely free** buckets
(`BCH_DATA_free`).  On a fragmented filesystem:
- 1000 buckets, each 50% used → `actual_free` = 2 GB
- But: 0 completely-free buckets
- Copygc consolidates partially-used buckets → creates free ones
- The disk reservation ensures `actual_free >= swap_size`, but all
  that free space could be distributed across partially-used buckets

Under sustained swap pressure:
1. Swap COW writes consume one free bucket per write point
2. Old blocks are freed, making those buckets partially-free (not
   completely free — other extents may share the bucket)
3. Copygc consolidates, but needs I/O bandwidth (competing with swap)
4. If copygc can't keep up: free bucket count → 0 → allocation fails

The disk reservation prevents this from getting WORSE (normal writers
are throttled), but doesn't prevent the initial condition.

### When this matters

- Near-full filesystem (>90% used) with high fragmentation
- Sustained heavy swap (not just a brief burst)
- Slow disk or I/O contention preventing copygc from running

### Mitigation

The reservation size helps here: a larger reservation means more free
space overall, giving copygc more room to consolidate.  With a
`swap_size` reservation (the current approach), `actual_free >=
swap_size` — plenty of free space for copygc to work with.  A smaller
reservation (32 MB) would leave less copygc headroom.

This is actually an argument FOR the large reservation, not against it.

### The writeup says "structurally impossible"

Line 1368-1369: "ENOSPC during reclaim is structurally impossible."
This is too strong.  The reservation makes it extremely unlikely by
preserving plenty of free space for copygc.  But on a heavily
fragmented filesystem where copygc is I/O-starved, free-bucket
exhaustion is theoretically possible even with the reservation.

"Extremely unlikely given the reservation and normal copygc
operation" would be more accurate.

## V. Writeup's ENOSPC section has stale reasoning (Low)

Lines 1275-1283 explain the interaction between the swap_activate
reservation and the per-write ENOSPC check:

> "For swap files, both would draw from the same `sectors_available`
> pool ... causing the per-write check to fail ... The fix: swap
> writes skip `BCH_WRITE_check_enospc`."

This reasoning is stale.  We showed in v5 that `disk_sectors_delta` =
0 for 1:1 COW replacements, so `bch2_disk_reservation_add` is never
called and `check_enospc` is never evaluated.  The per-write check
CANNOT fail for net-0 overwrites regardless of `sectors_available`.

Skipping `check_enospc` is correct as belt-and-suspenders (in case
`disk_sectors_delta` > 0 from compression ratio changes or replica
count changes).  But the writeup should not present it as a fix for a
problem that doesn't exist in the normal path.

Suggested correction: "For 1:1 COW replacements, `disk_sectors_delta`
= 0, so the per-write ENOSPC check is never reached.  Swap writes
skip `BCH_WRITE_check_enospc` as defence-in-depth for edge cases
(compression ratio changes, replica count mismatches)."

## W. PF_MEMALLOC on swap reads: correct but worth documenting (Trivial)

`bch2_swap_rw` sets `memalloc_noreclaim_save()` for both reads and
writes.  For writes this is essential (we're in reclaim context).

For reads (page fault context, not reclaim), PF_MEMALLOC:
- Prevents read-path allocations from entering reclaim
- Reclaim would try to swap out other pages → potential deadlock if
  those writes compete for the same btree locks as the read
- So PF_MEMALLOC on reads is actually important for deadlock avoidance

This is correct.  The writeup doesn't explain why PF_MEMALLOC is set
for reads — worth a brief comment in the code or writeup.

## Summary

| Finding | Severity | Status |
|---------|----------|--------|
| v5's "32 KB" estimate | **Retracted** | Was conflating reservation gate (never reached) with physical gate |
| Reservation SIZE over-reserved | Low (was: Medium) | Over-reservation is conservative but also helps copygc — arguably a feature |
| Free-bucket exhaustion on fragmented FS (finding U) | Medium | New finding; disk reservation mitigates but doesn't eliminate |
| Writeup says "structurally impossible" | Low | Should say "extremely unlikely" |
| ENOSPC section has stale reasoning (finding V) | Low | `check_enospc` is never evaluated for net-0; fix is belt-and-suspenders |
| PF_MEMALLOC on reads (finding W) | Trivial | Correct for deadlock avoidance; worth a code comment |
| Interior btree nodes not pinned (finding Q, from v4) | Medium | Unchanged — code pins leaves only |
