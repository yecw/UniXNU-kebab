# Pending GFander patches (adapted to xnu-10063 / 4 KiB, not yet in series)

These were adapted from `GFander/KebabCPU` (xnu-8796, 16 KiB) by subagents and
verified to apply cleanly on our tree, but they are not in `patches/xnu/series`
because they need a decision or a loader change first.

| Patch | What it does | Why it is pending |
| --- | --- | --- |
| `adapt-0366.patch` | Discontiguous DRAM banks: `vm_first_phys = kebab_memory_metadata_base()`, `pa_valid()` = main window ∪ owned banks, physmap apertures per bank. Includes the 0365 header/capture it needs. | Our DarwinBoot writes no KEBMMAP handoff, so `owned_count == 0` and the patch is a runtime no-op. Needs loader work (`args + 0x480`, `KEBMMAP1` v2) or the `/chosen/darwinboot-memory-outside` FDT route. |
| `adapt-0367.patch` | pmap object keys pages by physical span (`pmap_page_object_offset`). | A no-op without 0366. Apply order `0367 → 0366` is verified. |
| `adapt-0367-after-0366.patch` | Same, rebased for `0366 → 0367` order. | Only if the order above is preferred. |
| `adapt-0041.patch` | Link address block alignment: keeps `0x4000` L2 offset, adds a compile-time assert, fixes the MacOSX branch addresses. | Our 4 KiB / 2 MiB layout is already aligned; the running address comes from the build script, not `MakeInc.def`. |
