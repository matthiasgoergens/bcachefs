# PR Self-Review Findings

## 1. Pre-fragmentation on by default at 4KB granularity — no guard for small systems

`vfs/swap.c:bch2_swap_prefragment` runs unconditionally at every `swapon`.
The writeup documents that 4KB granularity "destroys performance at 128-192M"
(5-16% pass rate), but there is no runtime warning, no `swap_noprefrag` toggle,
and no size guard.

Options:
- Add a `bch2_swap_prefrag_enabled` cmdline toggle (like the other two) and
  default it to off until the granularity is resolved
- Or add a minimum-RAM / maximum-swap-file-size guard before running it
- Or change to a larger granularity (64KB) as the default

## 2. Prefragmentation failure blocks swap activation entirely

`vfs/swap.c:756-760` — if `bch2_swap_prefragment` returns an error (e.g.
ENOSPC midway through splitting extents), `bch2_swap_activate` propagates the
error and the swap file is completely unusable.  Partial fragmentation is
better than no swap at all.  Consider: log a warning and continue without full
fragmentation rather than hard-failing.

## 3. `plan.markdown` and `pr-description.markdown` are in the kernel tree

Development artifacts.  Harmless for the internal WIP PR, but must be stripped
before upstream submission.

## 4. Minor: `bch2_swap_noreclaim_enabled` is `extern bool`

`vfs/swap.h:43` — global `extern bool` shared between `swap.c` and `write.c`
for the feature toggle.  A `static bool` with an inline accessor in the header
would be cleaner.  Not urgent for WIP.

---

# Adversarial Review: "O(log n) pre-reserve suffices" Claim

The writeup's Memory Analysis (lines 1147–1230) and open-issues section
(lines 938–947) together claim:

1. Without pre-frag, the btree grows O(swap_pages) **during reclaim** —
   bounded but problematic.
2. With pre-frag, per-operation memory is O(log n) and a pre-reserve of
   BTREE_MAX_DEPTH (4) × btree_node_size (256 KB) = **1 MB** suffices.
3. The existing `bc->nr_reserve` mechanism "could be grown at swapon time"
   to provide this pool.

Claim 1 holds.  Claims 2 and 3 have significant gaps.

## A. The O(swap_pages) claim without pre-frag: correct

The writeup correctly says "bounded at O(swap_pages) — not unbounded — but
the growth during reclaim is the problem."

Minor: "~3N keys after N writes" is more precisely ~2N+1 (each COW write to
the middle of a large extent produces 3 keys where there was 1 → net +2).
Off by ~50%, same O().

## B. The 1 MB pre-reserve estimate is 3–5× too small

The calculation: BTREE_MAX_DEPTH (4) × 256 KB = 1 MB.
This covers **one** root-to-leaf traversal in **one** btree.

A single swap COW write traverses at least three btrees:

| Btree | Why | Code path |
|-------|-----|-----------|
| BTREE_ID_extents | The COW extent replacement | `bch2_extent_update` → `__bch2_btree_iter_traverse` |
| BTREE_ID_inodes | i_sectors update — **always** ("always have to do an inode update for fsync to work properly", data/write.c:336) | `bch2_extent_update_i_size_sectors` → `bch2_btree_iter_peek_slot` on cached inode iter |
| BTREE_ID_alloc or BTREE_ID_freespace | Block allocation when open bucket exhausted | `bch2_bucket_alloc_trans` → `for_each_btree_key_max_norestart(BTREE_ID_freespace)` |

With replication > 1, the alloc traversal repeats per replica.

Minimum for single-replica: **3 btrees × 4 levels × 256 KB = 3 MB**.

With mempool=8 concurrent transactions, worst case is 8 × 3 MB = 24 MB
(though cache sharing of hot root/interior nodes reduces this in practice).
But "1 MB" is not a safe lower bound even for a single concurrent write.

The writeup's qualifier "sufficient for any single path traversal" is
literally true — but a swap write is not a single traversal.  The document
reads as though 1 MB covers a whole swap write, which it doesn't.

## C. `bc->nr_reserve` is the wrong mechanism

The writeup (line 946–947) says: "The btree cache already has a reserve
mechanism (`bc->nr_reserve`) that could be grown at swapon time."

`bc->nr_reserve` controls the **eviction watermark**: it prevents the shrinker
from freeing the last N nodes from the non-pinned cache list.  It does NOT
pre-allocate page frames for reading nodes that are not in cache.

When a needed btree node has been evicted:

1. `bch2_btree_node_fill` → `bch2_btree_node_mem_alloc`
2. Tries `GFP_NOWAIT` (fast, non-blocking) — fails under pressure
3. Falls back to `GFP_KERNEL` after `bch2_trans_unlock_long`
4. Under PF_MEMALLOC: hits emergency reserves; if exhausted,
   `__GFP_NOFAIL` spins forever

Growing `bc->nr_reserve` keeps more already-cached nodes from being evicted —
useful defence in depth — but it provides zero help when the needed node is
**not in cache at all** and must be read from disk into a newly-allocated
buffer.

The proposed "private pool for btree node I/O during swap writes" is a
different mechanism: a set of pre-allocated page-sized buffers that the
btree read path can draw on under PF_MEMALLOC, bypassing the page allocator
entirely.  `bc->nr_reserve` cannot serve this purpose.  They are
complementary, not interchangeable.

## D. "No btree growth" is true for net key count, not physical node size

The Memory Analysis says "1:1 key replacement — no new keys, no node splits,
no btree growth" and presents runtime memory as cleanly O(log n).

The writeup's own adversarial section 1 (lines 541–587) correctly identifies
dead-key accumulation: if the old key is in a written (immutable) bset, it
becomes dead space and the new key is appended to the dirty bset.  After
overwriting all keys in a node: 50% dead + 50% live = 100% full.

These two sections are inconsistent.  The Memory Analysis reads as though
pre-frag gives constant-memory btree nodes.  Section 1 says nodes can
drift to 100% capacity between compactions.  Both can't be the full picture
simultaneously.

The correct framing: **net key count** is constant (O(log n) per-op working
set), but **physical node occupancy** drifts upward until journal reclaim
compacts.  The 1/3 headroom from pre-splitting handles this in steady state,
but the Memory Analysis should carry this caveat rather than implying the
btree is perfectly static.

## E. "No splits with pre-frag" holds — with caveats already in the document

Valid provided:
- New key has the same `u64s` as the old (same device count, checksum type —
  holds for common single-device configs)
- Node stays below 100% (dead-key accumulation can violate this; section 1)
- No foreign-file keys in the same leaf node (section 5)

All three caveats are covered in the adversarial sections.  The Memory
Analysis doesn't carry them forward, which makes it read stronger than it is.

## F. Alloc btree "does not grow" — correct

"Each bucket has exactly one alloc key; COW writes only modify sector counters
in existing keys."  Verified: open-bucket allocation doesn't write the alloc
btree; bucket close and block freeing modify existing keys via the key cache.
Net alloc key count is constant.

However, the alloc btree still needs to be **traversed** during block
allocation (point B above), so its nodes need to be in cache.  The current
code pins all alloc nodes (POS_MIN to SPOS_MAX), which handles this.  The
"pre-reserve O(log n)" alternative would need to account for alloc btree
cache misses too.

## Summary (v1)

| Claim | Verdict |
|-------|---------|
| Without pre-frag: O(swap_pages) growth during reclaim | **Correct** |
| With pre-frag: O(log n) per traversal | **Correct** |
| Pre-reserve 1 MB suffices | **3–5× underestimate** — 3 btrees minimum |
| `bc->nr_reserve` is the right mechanism | **Wrong** — prevents eviction, does not provide read-path page allocation |
| No btree growth with pre-frag | **True for net key count; false for physical node size** (dead-key accumulation, covered in section 1 but not in Memory Analysis) |
| No splits with pre-frag | **True in common case**, with section 1/5 caveats |
| Alloc btree does not grow | **Correct**, but alloc btree traversals still need cache coverage |

---

# Adversarial Review v2: Updated Writeup

The writeup has been significantly improved.  The three main findings from
v1 (1 MB underestimate, wrong mechanism, missing dead-key caveat) are all
addressed.  This review focuses on what's still wrong or incomplete.

## G. `bc->freeable` pre-allocation is not shrinker-safe

The updated open-issues section (lines 938–963) proposes: pre-allocate
btree node buffers at swapon, put them on `bc->freeable`, and bump
`nr_reserve` to protect them.

The shrinker (`bch2_btree_cache_scan`, cache.c:592) scans `bc->freeable`
**first**, before touching `live[0]`.  It unconditionally skips the first
3 entries (cache.c:597: `if (++i <= 3) continue`), then frees the rest
up to its budget.

`nr_reserve` only affects the budget calculation for `live[0]`
(`can_free = live[0].nr - nr_reserve`).  It does not directly limit how
many `freeable` nodes the shrinker can drain.  The total scan budget
(`nr_to_scan` from the kernel shrinker framework) is derived from the
`count` callback, which returns `btree_cache_can_free(live[0])`.

So the protection is **indirect**: bumping `nr_reserve` reduces the
shrinker's reported freeable count, which makes the kernel give a
smaller `nr_to_scan`, which limits how much `freeable` is drained.

This works well in the **small-memory scenarios** we care most about
(128–256M RAM): `live[0]` is small, `nr_reserve` bump makes
`can_free` near zero, the kernel gives a tiny scan budget, and
`freeable` survives.  But in larger-memory scenarios where
`live[0].nr >> nr_reserve`, the budget is large and most pre-allocated
buffers can be drained.

One saving grace: `list_add` (cache.c:144) adds to the **head**.
Pre-allocated nodes are at the head.  The "skip first 3" rule
protects the 3 most recently added entries (the head).  The allocation
path (`bch2_btree_node_mem_alloc`, cache.c:918) also iterates from the
head — so the 3 shrinker-protected nodes are the same 3 the allocator
tries first.  This means **at least 3 pre-allocated buffers (768 KB)
survive any amount of shrinker pressure**.  Enough for ~1 btree
traversal, but not a full swap write (which needs ~3 traversals).

**Recommendation**: for a robust solution, a dedicated swap reserve
list (separate from `bc->freeable`, never touched by the shrinker)
would be more reliable.  The `freeable` approach is workable as a
first implementation, especially in the small-memory case, but the
writeup should acknowledge that the shrinker protection is indirect
and partial.

## H. "Better than pinning" framing is misleading

The section title (line 938) says "Better than pinning" and argues
"can't know which ones will be needed ahead of time."

But the current code already pins:
- Extent btree leaves for the swap file's key range (**known**)
- Inode btree node for the swap inode (**known**)
- All alloc btree nodes (**overbroad but safe**)

For extents and inodes, pinning is correct and cheap — we DO know
which nodes are needed.  Pre-reserve is only "better" for the alloc
btree (where pinning everything is wasteful on large filesystems).

The text reads as though pre-reserve replaces all pinning.  In
practice it's complementary: pin the known nodes (extents, inodes),
pre-reserve for the unpredictable ones (alloc under foreground
allocation fallback).

## I. The 3 MB estimate overcounts: inode btree uses key cache

Both inode lookups in the write path use `BTREE_ITER_cached`
(data/write.c:227, data/write.c:1383).  After the first access, the
inode value lives in the in-memory key cache hash table — subsequent
reads do NOT traverse btree nodes or hit the btree cache.

So in steady state the btrees that need node I/O per swap write are:
- BTREE_ID_extents: up to 4 levels
- BTREE_ID_alloc/freespace: up to 4 levels (only when open bucket
  exhausted)

Practical per-write: **2 × 4 × 256 KB = 2 MB**, not 3 MB.

The 3 MB estimate is conservative — safe for sizing, but the writeup
should note the inode key cache optimization to avoid confusing future
readers into thinking all 3 btrees always need cold-path I/O.

## J. "~3N keys" still says ~3N (minor, from v1)

Line 1180–1181 still says "~3N keys instead of ~1."  Should be
~2N+1 (net +2 per write, not +3).  Off by a constant factor; same
O().  Minor.

## K. "Pre-splitting gives ~1/3 headroom" is ambiguous

Line 1199–1200: "Pre-splitting gives ~1/3 headroom to absorb this
drift in steady state."

1/3 of what?

- If pre-split to **50%**: headroom to 100% is 50%, and the dead-key
  worst case (overwrite all keys) exactly fills it → 0% margin.
- The **split threshold** headroom is 100% − 66% = 33% ≈ 1/3 — but
  that's the headroom in an *unsplit* node above the split threshold,
  which is a different quantity.

The sentence conflates two different headroom measures.  With 50%
pre-split, the correct statement is "headroom is 50% of node capacity,
which is exactly consumed by the dead-key worst case, leaving zero
margin."  The adversarial section 1 says this correctly; the Memory
Analysis should match.

## Summary (v2)

The updated writeup fixed the three main issues from v1.  Remaining:

| Finding | Severity | Status |
|---------|----------|--------|
| `bc->freeable` shrinker protection is indirect/partial | Medium | New mechanism proposed but protection not watertight |
| "Better than pinning" framing | Low | Misleading — pre-reserve complements pinning, doesn't replace it |
| 3 MB overcounts (inode key cache) | Low | Conservative overestimate, safe but worth noting |
| "~3N keys" should be ~2N+1 | Trivial | Uncorrected from v1 |
| "1/3 headroom" ambiguous | Low | Conflates pre-split headroom with split-threshold headroom |
