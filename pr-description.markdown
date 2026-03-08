# bcachefs: swap file support via SWP_FS_OPS

## Summary

Add swap file support for bcachefs using the `SWP_FS_OPS` path (same
mechanism as NFS swap).  The filesystem stays in the I/O loop for swap
operations, so swap data gets checksumming, encryption, replication,
compression, and multi-device support — all the features bcachefs
provides for regular file data.

Unlike btrfs (which disables COW, checksums, and compression for swap
files), bcachefs swap files use the normal COW write path.

## What it does

- `swapon /path/to/bcachefs/swapfile` just works
- Swap I/O goes through bcachefs direct I/O (COW writes)
- Btree nodes are pinned at swapon to prevent cache eviction deadlocks
- `PF_MEMALLOC` prevents reclaim re-entry during swap writes
- swapoff/swapon cycling works correctly

## How to use

```bash
# Create a swap file on an existing bcachefs filesystem
fallocate -l 4G /swapfile
dd if=/dev/zero of=/swapfile bs=1M conv=notrunc   # write extents
chmod 600 /swapfile
mkswap /swapfile
swapon /swapfile
```

Works with compression, multi-device tiered storage, and all other
bcachefs features.  Recommended to also have a small raw swap partition
or zram as a safety net for extreme memory pressure.

## Implementation

Three new callbacks in `bch_address_space_operations`:

**`bch2_swap_activate`**: validates the file, pins btree nodes
(extents, inodes, alloc) to prevent cache eviction during swap I/O,
sets `SWP_FS_OPS`.

**`bch2_swap_deactivate`**: unpins btree nodes.

**`bch2_swap_rw`**: sets `IOCB_DIRECT` and `PF_MEMALLOC` (via
`memalloc_noreclaim_save`), then routes through `bch2_read_iter` /
`bch2_write_iter`.

Changes to existing code:

**`bch2_direct_write`** (vfs/direct.c): skip `generic_write_checks`,
`inode_lock`, `file_update_time`, `bch2_pagecache_block_get`, and
pagecache invalidation for swap files (`IS_SWAPFILE` check).

**`__bch2_write`** (data/write.c): upgrade `PF_MEMALLOC_NOFS` to
`PF_MEMALLOC` for swap ops (`BCH_WRITE_swap` flag), preventing
direct reclaim from all allocations in the write path.

**`bch2_write_point_do_index_updates`** (data/write.c): propagate
`PF_MEMALLOC` to the workqueue thread for swap ops.

**`BCH_WRITE_swap`** (data/write_types.h): new write flag that
identifies swap I/O ops throughout the write path.

## The reclaim deadlock and how we fix it

The core challenge: swap writes happen during memory reclaim.  If any
allocation in the write path tries to reclaim memory, that reclaim
tries to swap more pages, calling back into bcachefs → infinite loop.

We confirmed this via GDB — the deadlock was in the btree transaction
path where `bch2_printbuf_make_room` calls `krealloc(GFP_KERNEL)`,
entering direct reclaim inside a workqueue thread that doesn't inherit
the caller's `PF_MEMALLOC` flags.

The fix has two parts:
1. `memalloc_noreclaim_save()` in `bch2_swap_rw` (protects the calling
   thread)
2. `BCH_WRITE_swap` flag + `PF_MEMALLOC` in the write index worker
   (protects the workqueue thread)

## Test results

Tested in QEMU VMs with constrained RAM and a Rust-based init that
creates memory pressure by gradually faulting pages:

| Configuration | RAM | Swap Used | Result |
|---------------|-----|-----------|--------|
| Single disk, no compression | 128M | 196 MB | PASS |
| Single disk, no compression | 192M | 256 MB | PASS |
| Single disk, LZ4 | 192M | 202 MB | PASS |
| Single disk, ZSTD | 192M | 96 MB | PASS |
| Multi-device tiered, ZSTD | 192M | 145 MB | PASS |
| Swapoff/swapon cycle | 192M | — | PASS |
| Large FS (10K files) + raw swap | 128M | 199 MB | PASS |

### Ablation testing

Feature toggles via kernel cmdline (`bcachefs.swap_nopin`,
`bcachefs.swap_noreclaim`) to verify each feature is necessary:

Small filesystem (2G, few btree nodes):

| PF_MEMALLOC | Pin | Result | Notes |
|-------------|-----|--------|-------|
| ON | ON | PASS | |
| ON | OFF | PASS | Pinning not needed when btree is small |
| OFF | ON | **HUNG** | PF_MEMALLOC is necessary |
| OFF | OFF | **HUNG** | |

Large filesystem (4G, 10K files, many btree nodes):

| PF_MEMALLOC | Pin | drop_caches | Result | Notes |
|-------------|-----|-------------|--------|-------|
| ON | ON | OFF | PASS/flaky | At edge of emergency reserves |
| ON | OFF | OFF | PASS | |
| ON | ON | ON | PASS | Pinning saves from cache eviction |
| ON | OFF | ON | **HUNG** | Cache miss deadlock without pinning |

Conclusion:
- **PF_MEMALLOC**: always required (deadlocks without it)
- **Btree node pinning**: required when btree nodes are evicted
  (reproducible with drop_caches, possible under sustained pressure)
- **Raw swap safety valve**: prevents PF_MEMALLOC reserve depletion;
  recommended for production

## Files changed

```
 fs/bcachefs/Makefile           |   1 +
 fs/bcachefs/data/write.c       |  11 +++
 fs/bcachefs/data/write_types.h |   1 +
 fs/bcachefs/vfs/direct.c       |  30 ++++--
 fs/bcachefs/vfs/fs.c           |   4 +
 fs/bcachefs/vfs/swap.c         | 153 +++++++++++++++++++++++++++++++++
 fs/bcachefs/vfs/swap.h         |  14 +++
 7 files changed, 207 insertions(+), 7 deletions(-)
```

## Known limitations / future work

- No pre-fragmentation of extents yet (would improve btree headroom
  under sustained swap pressure)
- No deferred btree splits under memory pressure
- No dedicated swap btree (shared extents btree, relies on
  `PF_MEMALLOC` for deadlock avoidance)
- `PF_MEMALLOC` uses emergency memory reserves — under extreme
  pressure, could deplete reserves.  A small raw swap partition as
  safety net is recommended.
- Btree node pinning pins all alloc btree nodes, which could be
  significant on large filesystems.  Lazy pinning is a future
  optimization.

## Design document

Full design rationale, adversarial review, and implementation journey
in `writeup.markdown` in this branch.
