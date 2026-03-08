# bcachefs Swap Implementation and Bugfixes

This document summarizes the investigation, bugfixes, and stress test infrastructure additions made to support swap files on bcachefs.

## Core Issues Fixed

### Bug 1: Swap module not activating
- **Issue**: `bch2_swap_activate` wasn't being called when running `swapon` despite the DKMS module being installed.
- **Root Cause**: The system's root partition (`/`) is bcachefs. This means the stock `bcachefs.ko` module was being loaded early from the `initramfs` during boot, completely shadowing the custom DKMS version that contained the new swap code.
- **Fix**: Rebuilt the initramfs with `mkinitcpio -P` to explicitly pack the new DKMS module. After rebooting, `swapon /swapfile` succeeded.

### Bug 2: `fallocate` swap files rejected by the kernel
- **Issue**: Swap files created over unwritten extents via `fallocate` were rejected by the kernel because the generic `bmap()` handler returned 0 for holes.
- **Root Cause**: Bcachefs's custom swap implementation (`SWP_FS_OPS`) was being bypassed or misunderstood by standard generic VFS logic previously.
- **Fix**: Once the custom module was properly loaded (Bug 1), the `bch2_swap_activate` function ran. This explicitly leverages `SWP_FS_OPS`, telling the kernel that the entire file is a single contiguous block. Bcachefs dynamically handles logical-to-physical block translations for extents within the btree on the fly during I/O. Therefore, `fallocate` just works seamlessly.

### Bug 3 & 5: Severe Foreground I/O Stalls
- **Issue**: Heavy sequential background writeback (such as creating an 8GB swapfile via `dd` or `fallocate` + syncing) caused foreground commands like `ls`, `stat`, and `sync` to hang for multiple minutes.
- **Root Cause**: Bcachefs uses a sequential bypass optimization on tiered storage. Giant sequential writes bypass the fast `foreground_target` (NVMe) and write directly to the `background_target` (HDDs). During these slow physical disk writes, the VFS and btree threads suffered from read/write locking starvation, causing them to hold the SRCU read lock excessively long.
- **Fix**: The custom SRCU unlock fixes backported onto this branch (`drop_locks_long_do()`) completely resolved this. A live test involving a 4GB background `dd` and concurrent `time ls` showed the foreground operations returning in 0.002 seconds with no kernel stall warnings.

## Stress Test Infrastructure (Bug 4)

We extracted the `test-vm` into a standalone repository: `~/prog/bcachefses/stress-tests` to ensure regressions are automatically caught.

The Rust-based QEMU init program (`vm-init-rs`) was significantly enhanced:
1. **Native Rust Implementations**: Added native `mkswap`, `fallocate`, and `dd` logic inside the Rust binary. This avoids dependency on busybox or host binary bundling.
2. **Matrix Testing**: The `run-vm-test.sh` script now supports three distinct torture scenarios passed via the kernel boot string:
    - **`partition`**: The baseline test over raw block devices.
    - **`dd`**: Zeroes out 512MB of file blocks synchronously, writes the swap signature, activates swap, and subjects it to extreme memory pressure.
    - **`fallocate`**: Explicitly preallocates a file using unwritten extents, writes the swap signature, activates swap, and tests stability during memory thrashing.
3. All tests passed successfully inside the VM under 256MB memory constraints.

## System Configuration Notes for Desktop (`fstab`)

Your desktop is currently running the custom bcachefs module perfectly.

Regarding your `/etc/fstab` and mount options:
1. You have `/swapfile file 16G` active. This is now fully supported on bcachefs.
2. You also have two dedicated raw swap partitions in your `/etc/fstab` (one on NVMe, one on HDD). If you intend to purely use the bcachefs swapfile moving forward, you may wish to comment these out to consolidate memory management to the filesystem swap.
3. Your bcachefs root is mounted with `rw,relatime`. The `fsync=false` or similar performance lies you mentioned earlier might still affect data durability during power loss, but they are no longer causing locking stalls inside bcachefs itself now that the SRCU patches are active.
