# Why a `MEMCreate` GDAL C API is a reasonable addition

## Where this API is useful

This API is useful in analytical database workloads that process many small raster tiles concurrently.

For example, in SedonaDB, some raster operations such as clipping need to create temporary MEM datasets on the hot path. In that kind of workload, the overhead of the currently available public entry points becomes visible:

- `GDALCreate()` with the MEM driver
- `GDALOpen()` / `GDALOpenEx()` on `MEM:::` strings

## 1. It is useful

`MEMDataset::Create()` is already widely used throughout GDAL itself, which is a strong signal that the operation is broadly useful.

Examples include:

- raster IO helper paths in `gcore/rasterio.cpp:1089`
- cutline and raster mask workflows in `alg/gdalcutline.cpp:340`
- pansharpening workflows in `alg/gdalpansharpen.cpp:1301`
- raster tiling workflows in `apps/gdalalg_raster_tile.cpp:1372`

So this is not a downstream-only need. GDAL itself already relies on direct MEM dataset construction in a number of performance-sensitive paths.

## 2. It is efficient

Benchmark results for `create_add_band`, `512x512x1 Byte`:

| workload | mem_cpp | mem_driver_create | mem_open | mem_open_internal |
|---|---:|---:|---:|---:|
| single-thread | 808,464.62 ops/s | 549,629.23 ops/s | 82,098.45 ops/s | 80,572.15 ops/s |
| concurrent (8 threads) | 2,174,564.29 ops/s | 435,688.96 ops/s | 89,589.55 ops/s | 92,501.53 ops/s |

### 2.1 Why `mem_cpp` is faster than `GDALCreate(MEM, ...)`

`GDALCreate(MEM, ...)` goes through the driver create path and registers the dataset in GDAL's global open-dataset list. That adds extra create-time and close-time bookkeeping.

By contrast, direct `MEMDataset::Create()` avoids that registration path.

This matters even more under concurrency, because the global open-dataset list introduces shared mutable state and additional contention.

### 2.2 Why `mem_cpp` is faster than `GDALOpen("MEM:::")`

`GDALOpen("MEM:::")` goes through the generic GDAL open path.

That means it pays for work that a direct MEM constructor does not need:

- driver lookup and probing
- `GDALOpenInfo` construction
- file-style open/stat probing
- open-option validation

Profiling shows this generic open-path overhead is the main reason `mem_open` is slow. Open-dataset registration is only a smaller additional cost on top of that.

### 2.3 Why the direct path scales better

In the 8-thread benchmark, the direct path is about `4.99x` faster than `GDALCreate(MEM, ...)`.

That gap is larger than in the single-thread case because the direct path avoids both:

- global open-dataset registration overhead
- generic open-path locking and probing overhead

Overall, direct MEM creation has less overhead and scales better in multi-threaded workloads that repeatedly construct temporary MEM datasets.

## Why a public C API helps

Today, a downstream project that wants this fast path must either:

- use `GDALCreate()` and accept the extra registration overhead
- use `GDALOpen("MEM:::")` and accept the generic open-path overhead
- call the private C++ `MEMDataset::Create()` symbol directly, which is fragile and not a stable public interface

A public `MEMCreate(...)` C API would provide a stable and intentional way to expose functionality that is already useful inside GDAL and measurably more efficient for high-concurrency analytical workloads.
