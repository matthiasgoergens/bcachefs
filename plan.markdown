# Implementation Plan: Phase 1 — Minimal COW Swap

Goal: get `swapon` working on a bcachefs file using `SWP_FS_OPS`.

## What we're building

Three VFS callbacks wired into `bch_address_space_operations`:

1. **`bch2_swap_activate`** — called by `swapon(2)`. Validates the file,
   sets `SWP_FS_OPS`, returns extent count.
2. **`bch2_swap_deactivate`** — called by `swapoff(2)`. Cleanup.
3. **`bch2_swap_rw`** — called for every swap read/write. Routes through
   bcachefs direct I/O.

## Files to create/modify

### New: `fs/bcachefs/vfs/swap.c`

```c
// bch2_swap_activate:
//   - Validate: regular file, not inline, has extents
//   - sis->flags |= SWP_FS_OPS
//   - add_swap_extent(sis, 0, sis->max, 0)
//   - return 1

// bch2_swap_deactivate:
//   - no-op for now (cleanup in phase 2)

// bch2_swap_rw:
//   - if READ: call bch2_read_iter(iocb, iter)
//   - if WRITE: call bch2_write_iter(iocb, iter)
//   - return 0 on success, error on failure
```

**Key decision**: `bch2_swap_rw` needs to route through existing I/O paths.

Problem: `bch2_direct_IO_read` is `static` in direct.c. And
`bch2_direct_write` takes inode_lock and does write checks that aren't
needed for swap.

Options:
- **A**: Use `bch2_read_iter` / `bch2_write_iter` (the public file_operations
  entry points). These handle both buffered and direct I/O and do their own
  locking. But swap I/O should bypass the page cache — it's already direct.
- **B**: Export `bch2_direct_IO_read` and create a simplified
  `bch2_swap_write` that skips inode lock / generic_write_checks / privs.
- **C**: Write a minimal swap-specific I/O path that submits bios directly,
  using the write_op machinery.

For phase 1, **option A** is simplest — just call the existing iterators.
The kiocb from the kernel swap subsystem already has the right file and
position set. If performance matters, we can optimize later.

### New: `fs/bcachefs/vfs/swap.h`

```c
#ifndef _BCACHEFS_VFS_SWAP_H
#define _BCACHEFS_VFS_SWAP_H

int bch2_swap_activate(struct swap_info_struct *, struct file *, sector_t *);
void bch2_swap_deactivate(struct file *);
int bch2_swap_rw(struct kiocb *, struct iov_iter *);

#endif
```

### Modified: `fs/bcachefs/vfs/fs.c` (3 lines)

Add to `bch_address_space_operations`:
```c
.swap_activate  = bch2_swap_activate,
.swap_deactivate = bch2_swap_deactivate,
.swap_rw        = bch2_swap_rw,
```

### Modified: `fs/bcachefs/Makefile` (1 line)

Add `vfs/swap.o` to bcachefs-y.

## What swap_rw actually receives

From mm/page_io.c, the kernel calls `swap_rw` like this:

```c
// For writes (swap_write_unplug):
iov_iter_bvec(&from, ITER_SOURCE, sio->bvec, sio->pages, sio->len);
ret = mapping->a_ops->swap_rw(&sio->iocb, &from);

// For reads (swap_read_folio_fs):
iov_iter_bvec(&to, ITER_DEST, &bv, 1, folio_size(folio));
ret = mapping->a_ops->swap_rw(&sio->iocb, &to);
```

The `kiocb` is initialized with `init_sync_kiocb` on the swap file, with
`ki_pos` set to the byte offset in the file. The `iov_iter` wraps bvecs
pointing to the swap pages.

Return: 0 or -EIOCBQUEUED on success, negative error otherwise.

## Verification

1. Build kernel with bcachefs
2. Create bcachefs filesystem on a device/file
3. Create a regular file, fallocate + dd zeros + mkswap
4. swapon the file
5. Verify with `swapon --show`
6. Run a simple memory hog to force swapping
7. swapoff

## What's NOT in phase 1

- Pre-fragmentation (extents stay as-is)
- Node pinning (rely on btree cache for now)
- Deferred splits
- Proactive compaction
- Reserved inode bit
- VM test harness

These are all phase 2+. Phase 1 just proves the plumbing works.
