# Spin-off: GFP_NORETRY for btree node pre-allocation

## The bug

`__bch2_btree_node_mem_alloc` hardcodes `GFP_KERNEL` for all three
allocations (struct btree, data buffer via kvmalloc, aux_data via
kvmalloc).  Each data buffer is `btree_node_size` bytes (default
256 KB).

Any caller that wants best-effort allocation (try but don't OOM) has
no way to express that.  `bch2_btree_cache_add_reserve` allocates many
nodes in a loop and already handles partial allocation (returns count
allocated, callers check), but the underlying kvmalloc enters aggressive
reclaim and OOM before returning NULL.

On a memory-constrained system (128–192M VM after heavy I/O), allocating
64 × 256 KB = 16 MB of btree node buffers via GFP_KERNEL triggers:

    __kvmalloc_node_noprof → __alloc_pages_slowpath → out_of_memory
    → "System is deadlocked on memory" → panic

## The fix

Add `__bch2_btree_node_mem_alloc_gfp(c, gfp)` — same as the existing
function but takes a `gfp_t` parameter.  The existing
`__bch2_btree_node_mem_alloc` becomes a one-line wrapper passing
`GFP_KERNEL`.

`bch2_btree_cache_add_reserve` switches to calling the `_gfp` variant
with `GFP_KERNEL | __GFP_NORETRY`.  This makes kvmalloc try once
without aggressive reclaim or OOM kill.  On failure it returns NULL,
the loop breaks, the caller gets a partial reserve.

No change for any other caller of `__bch2_btree_node_mem_alloc`.

## Source commit

Cherry-pick from `65ed5ee243f0` on the `swap-files` branch of
`matthiasgoergens/bcachefs`.  The relevant diff is only in
`fs/bcachefs/btree/cache.c` — extract that file's changes and amend
the commit message to remove swap references.

Relevant hunks:
- Forward declaration of `__bch2_btree_node_mem_alloc_gfp` (near line 80)
- `bch2_btree_cache_add_reserve`: change `__bch2_btree_node_mem_alloc(c)`
  to `__bch2_btree_node_mem_alloc_gfp(c, GFP_KERNEL | __GFP_NORETRY)`
- New function `__bch2_btree_node_mem_alloc_gfp` (near line 290)
- `__bch2_btree_node_mem_alloc` rewritten as a wrapper

## How to reproduce (without swap)

The bug triggers when many btree node buffers are allocated on a
memory-constrained system.  To reproduce without any swap code:

### 1. Temporarily add a sysfs trigger

Apply this patch to `fs/bcachefs/btree/cache.c` (on top of a clean
tree, before the fix):

```c
// In bch2_btree_cache_alloc (or add a debugfs/sysfs entry):
// Force-allocate 64 btree nodes to simulate the reserve path
static ssize_t test_btree_reserve(struct bch_fs *c)
{
    int n = bch2_btree_cache_add_reserve(c, 64);
    pr_info("bcachefs: allocated %d/64 btree reserve nodes\n", n);
    return 0;
}
```

Or just call `bch2_btree_cache_add_reserve(c, 64)` from an existing
debugfs entry or module parameter handler.

### 2. Create a constrained VM

```bash
# Create a small bcachefs disk
truncate --size=2G /tmp/test-disk.img
LOOP=$(sudo losetup --find --show /tmp/test-disk.img)
sudo bcachefs format "$LOOP"
sudo mkdir -p /mnt/test
sudo mount -t bcachefs "$LOOP" /mnt/test

# Create files to populate the btree (uses memory for btree nodes)
for i in $(seq 1 5000); do
    sudo dd if=/dev/urandom of="/mnt/test/file$i" bs=4K count=1 \
        status=none 2>/dev/null
done
sudo sync
```

### 3. Run in a memory-constrained QEMU VM

```bash
qemu-system-x86_64 \
    -enable-kvm -m 128M -smp 2 -nographic -no-reboot \
    -kernel /path/to/bzImage \
    -initrd /path/to/initramfs \
    -append "console=ttyS0 panic=1" \
    -drive file=/tmp/test-disk.img,format=raw,if=virtio
```

The init should mount the filesystem, consume most of the RAM (e.g.
with a malloc loop), then trigger the btree reserve allocation.

### 4. Expected results

**Without the fix**: `__alloc_pages_slowpath` → OOM → "System is
deadlocked on memory" → kernel panic.

**With the fix**: `bch2_btree_cache_add_reserve` returns a partial
count (e.g. 40/64), no OOM, no panic.  The kernel log shows
"allocated N/64 btree reserve nodes" with N < 64.

## Commit message for the standalone PR

```
bcachefs: use GFP_NORETRY for best-effort btree node allocation

Add __bch2_btree_node_mem_alloc_gfp() that takes a gfp_t parameter,
allowing callers to request non-aggressive allocation.

bch2_btree_cache_add_reserve() switches to GFP_KERNEL|__GFP_NORETRY.
This function allocates many 256KB btree node buffers in a loop and
already handles partial allocation, but the underlying kvmalloc could
enter aggressive reclaim and OOM before returning NULL.  On a
memory-constrained system, this causes "System is deadlocked on
memory" panics.

With __GFP_NORETRY, kvmalloc tries once without aggressive reclaim
or OOM.  On failure the loop breaks and the caller gets a partial
reserve — the intended behavior.
```
