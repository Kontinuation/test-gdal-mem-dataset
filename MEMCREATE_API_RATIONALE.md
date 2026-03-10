# Why a `MEMCreate` GDAL C API is a reasonable addition

# Background

`MEMDataset::Create` is useful in analytical database workloads that process many small raster tiles concurrently.

For example, in [SedonaDB](https://github.com/apache/sedona-db), some raster operations such as clipping need to create temporary MEM datasets on the hot path. In that kind of workload, the overhead of the currently available public entry points becomes visible:

- `GDALCreate()` with the MEM driver
- `GDALOpen()` / `GDALOpenEx()` on `MEM:::` strings

In order to make MEM dataset creation more efficient for that kind of workload, we hacked a little bit by finding the mangled name of `MEMDataset::Create()` and calling it directly from our Rust code (See [PR #681 in sedona-db](https://github.com/apache/sedona-db/pull/681/changes#diff-a05409bc957ff871584c661dc4d51cd54c56144c13fb317e4942ea42fa782175R180-R193)). This is a pretty fragile approach, and it would be much better to have a stable public C API for this purpose.

# Rationale for a public C API

We believe that a public `MEMCreate(...)` C API is a reasonable addition to GDAL, not only for the use case of SedonaDB, but also for GDAL itself and other downstream projects. Here are the main reasons:

## 1. It is useful

`MEMDataset::Create()` is already widely used throughout GDAL itself, which is a strong signal that the operation is broadly useful.

Examples include:

- raster IO helper paths in [gcore/rasterio.cpp:1089](https://github.com/OSGeo/gdal/blob/v3.12.2/gcore/rasterio.cpp#L1089)
- cutline and raster mask workflows in [alg/gdalcutline.cpp:341](https://github.com/OSGeo/gdal/blob/v3.12.2/alg/gdalcutline.cpp#L341)
- pansharpening workflows in [alg/gdalpansharpen.cpp:1302](https://github.com/OSGeo/gdal/blob/v3.12.2/alg/gdalpansharpen.cpp#L1302)
- raster tiling workflows in [apps/gdalalg_raster_tile.cpp:1373](https://github.com/OSGeo/gdal/blob/v3.12.2/apps/gdalalg_raster_tile.cpp#L1373)

So this is not a downstream-only need. GDAL itself already relies on direct MEM dataset construction in a number of performance-sensitive paths.

## 2. It is efficient

We have a [repository](https://github.com/Kontinuation/test-gdal-mem-dataset) for testing the performance of various MEM dataset creation paths:

* `mem_cpp`: direct call to `MEMDataset::Create()`
* `mem_driver_create`: call to `GDALCreate("MEM", ...)`
* `mem_open`: call to `GDALOpen("MEM:::", ...)`
* `mem_open_internal`: call to `GDALOpenEx("MEM:::", ..., GDAL_OF_INTERNAL)`

Benchmark results for `create_add_band`, `512x512x1 Byte`:

| workload | mem_cpp | mem_driver_create | mem_open | mem_open_internal |
|---|---:|---:|---:|---:|
| single-thread | 808,464.62 ops/s (1.00x) | 549,629.23 ops/s (0.68x) | 82,098.45 ops/s (0.10x) | 80,572.15 ops/s (0.10x) |
| concurrent (8 threads) | 2,174,564.29 ops/s (1.00x) | 435,688.96 ops/s (0.20x) | 89,589.55 ops/s (0.04x) | 92,501.53 ops/s (0.04x) |

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

Profiling shows this generic open-path overhead is the main reason `mem_open` is slow. The driver lookup is the dominant contention spot when multiple threads are doing `GDALOpen("MEM:::")` concurrently.

# Conclusion

Today, a downstream project that wants this fast path must either:

- use `GDALCreate()` and accept the extra registration overhead
- use `GDALOpen("MEM:::")` and accept the generic open-path overhead
- call the private C++ `MEMDataset::Create()` symbol directly, which is fragile and not a stable public interface

A public `MEMCreate(...)` C API would provide a stable and intentional way to expose functionality that is already useful inside GDAL and measurably more efficient for high-concurrency analytical workloads, similar to what an existing C API `VRTCreate(...)` does for VRT datasets.
