# AuraOS — Phase A Foundation

This branch hardens the kernel foundation before user mode/process isolation.

## Implemented

- PMM validates the EFI memory map and protects page zero, the kernel image and its bitmap.
- PMM exposes page statistics and rejects invalid/double frees.
- Paging validates alignment and integer overflow.
- 2 MiB UEFI mappings are split into 4 KiB mappings before a single page is remapped.
- Huge-page mappings are handled explicitly by `paging_get_phys()` and rejected by normal unmap.
- VMM refuses to overwrite an existing mapping and rolls back partial allocations.
- Heap payloads are 16-byte aligned.
- Heap blocks have magic validation, exact-pointer validation and double-free detection.
- Added a GitHub Actions build + QEMU boot smoke test.

## Validation

The branch is intentionally kept separate from `main`. CI must compile the full UEFI image and boot it under QEMU/OVMF before this work is considered ready to merge.

## Remaining Phase-A items

- Wire the kernel panic module into the kernel build and exception path after the CI baseline is green.
- Add focused allocator/VMM tests that can run without a graphical session.
- Add stricter build warnings and reproducible toolchain checks.
