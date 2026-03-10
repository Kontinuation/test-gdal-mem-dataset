# Why a `MEMCreate` GDAL C API is a reasonable addition

This note explains why adding a public GDAL C API for direct MEM dataset creation is a reasonable design choice.

## Where this API is useful

This API is useful in analytical database workloads that perform raster processing on many small raster tiles concurrently.

For example, in SedonaDB we run raster operations on small raster tiles using multiple threads. Some operations, such as raster clipping, need to create temporary MEM datasets as part of the computation. In that kind of workload, MEM dataset construction is not an occasional setup step; it is on the hot path.

That makes the overhead of the existing public entry points visible:

- `GDALCreate()` with the MEM driver
- `GDALOpen()` / `GDALOpenEx()` on `MEM:::` strings

When thousands or millions of tiny MEM datasets are created concurrently, even small per-dataset overheads become important.

## Why add `MEMCreate`

There are two reasons.

## 1. It is useful

`MEMDataset::Create()` is already widely used throughout GDAL itself, which is a strong signal that the underlying operation is broadly useful.

Examples from the GDAL code base include:

- raster IO helper paths that wrap working buffers as temporary MEM datasets in `gcore/rasterio.cpp:1089`
- cutline and raster mask workflows in `alg/gdalcutline.cpp:340`
- pansharpening workflows that wrap extracted buffers in MEM datasets in `alg/gdalpansharpen.cpp:1301`
- raster tiling workflows in `apps/gdalalg_raster_tile.cpp:1372`

And that is only a subset. `MEMDataset::Create()` is also used in JPEG/JP2/HDF5/PDF/tile/vector and other internal code paths across the GDAL tree.

So this is not an artificial downstream-only need. GDAL itself already relies on this direct constructor in many places where creating a lightweight in-memory dataset is the right tool.

## 2. It is efficient

### 2.1 It is faster than existing public alternatives

The benchmark CLI in this repository compares four modes:

- `mem_cpp`: direct `MEMDataset::Create()`
- `mem_driver_create`: `GDALCreate()` with the MEM driver
- `mem_open`: `GDALOpen()` on `MEM:::`
- `mem_open_internal`: `GDALOpenEx()` on `MEM:::` with `GDAL_OF_INTERNAL`

For tiny create/destroy workloads, direct MEM creation is consistently faster than both `GDALCreate()` and `GDALOpen()`.

The open-string path is especially expensive because it goes through the generic GDAL open path.

### 2.2 It is faster under concurrency

The difference becomes more important when many threads create and destroy MEM datasets concurrently.

Profiling of the `tiny_concurrent` case shows two different kinds of extra overhead in the existing public APIs:

- `GDALCreate(MEM, ...)` pays for global opened-dataset registration and corresponding close-time bookkeeping
- `GDALOpen("MEM:::")` pays for the generic open path: driver lookup/probing, `GDALOpenInfo` construction, open/stat probing, and open-option validation; registration is only an additional smaller cost

By contrast, direct MEM creation avoids both classes of overhead.

Overall, this means direct MEM creation has less overhead and scales better in multi-threaded tiny-dataset workloads.

## What the profiler shows

The `tiny_concurrent` profiles are summarized in `TINY_CONCURRENT_PROFILE_FINDINGS.md`.

The key conclusions are:

- `mem_open` is slow mainly because of generic open-path overhead, especially driver lookup/probing and related locking
- `mem_open_internal` is somewhat faster than `mem_open`, which shows registration matters, but registration is not the main reason the open path is slow
- `mem_driver_create` is slower than direct MEM creation mainly because it inserts datasets into GDAL's global open-dataset list and pays the corresponding teardown cost on close
- `mem_cpp` is the fastest path because it avoids both the generic open path and open-dataset-list registration

## Why a public C API helps

Today, a downstream project that wants this fast path must either:

- use `GDALCreate()` and accept the extra registration overhead, or
- use `GDALOpen("MEM:::")` and accept the generic open-path overhead, or
- call the private C++ `MEMDataset::Create()` symbol directly, which is fragile and not portable as a public interface

A public `MEMCreate(...)` C API would solve that cleanly.

It would provide a stable and intentional way to access functionality that is:

- already useful inside GDAL
- already useful to downstream systems
- already demonstrably more efficient for high-concurrency analytical workloads

## Recommended positioning for upstream

The case for `MEMCreate` is not just "one more convenience wrapper".

It is a reasonable API because it exposes an existing, widely used, performance-relevant constructor through a stable C interface, much like `VRTCreate()` does for VRT datasets.

In short:

- it is useful because GDAL and downstream users both need direct lightweight MEM dataset construction
- it is efficient because it avoids avoidable overheads in the currently available public APIs
