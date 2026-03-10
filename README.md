# test-gdal-mem-dataset

Small C++ benchmark CLI for exploring the performance of different GDAL MEM dataset creation paths.

It compares:

- `mem_cpp` - direct call to the private C++ `MEMDataset::Create` symbol
- `mem_driver_create` - `GDALCreate()` on the `MEM` driver
- `mem_open` - string-based `GDALOpen("MEM:::")`
- `mem_open_internal` - string-based `GDALOpenEx("MEM:::", GDAL_OF_INTERNAL, ...)`

The extra `mem_open_internal` mode helps separate string/open-path overhead from global open-dataset-list registration overhead.

## Why this exists

This tool is intended to gather evidence for adding an upstream public C API for direct MEM dataset creation, similar in spirit to `VRTCreate()`.

## Building

This project requires GDAL development libraries.

```bash
make
```

## Usage

```bash
./gdal_mem_test --mode mem_cpp --operation create_only \
  --width 256 --height 256 --bands 1 --type Byte \
  --threads 8 --iterations 100000 --warmup 1000
```

### Common options

- `--mode <mem_cpp|mem_driver_create|mem_open|mem_open_internal|all>`
- `--operation <create_only|create_add_band|create_write_pixel|create_full>`
- `--width <int>`
- `--height <int>`
- `--bands <int>`
- `--type <GDAL type name>`
- `--threads <int>`
- `--iterations <int>`: iterations per thread
- `--warmup <int>`: warmup iterations per thread
- `--repeat <int>`
- `--interleave <band|pixel>`
- `--set-geotransform`
- `--set-projection`
- `--write-size <1x1|scanline|block>`
- `--block-width <int>`
- `--block-height <int>`
- `--json`
- `--csv`
- `--verbose`
- `--list-modes`
- `--list-types`

## Notes

- `mem_cpp` resolves the private `MEMDataset::Create` C++ symbol by mangled name at runtime. That is intentionally fragile, and mirrors the kind of workaround downstream projects currently need.
- `mem_open` modes automatically set `GDAL_MEM_ENABLE_OPEN=YES` so the MEM string open path is available.
- `mem_open` benchmarks wrapper creation around a preallocated caller-owned backing buffer. This matches the normal `MEM:::` use case better than allocating a fresh external buffer inside each timed iteration.
- `create_add_band` uses caller-owned backing buffers via `DATAPOINTER` so the benchmark focuses on dataset/band wiring overhead instead of repeated per-band heap allocation inside GDAL.
- Worker threads set frequently-read CPL options as thread-local config options before benchmarking to reduce contention from repeated global/environment config lookups.

## Example benchmark matrix

```bash
./gdal_mem_test --mode all --operation create_add_band --width 512 --height 512 \
  --bands 1 --type Byte --threads 1 --iterations 50000 --warmup 1000

./gdal_mem_test --mode all --operation create_add_band --width 512 --height 512 \
  --bands 1 --type Byte --threads 8 --iterations 20000 --warmup 1000
```

## Profiling

There is a helper script for macOS `sample`:

```bash
scripts/profile.sh 20 --mode mem_driver_create --operation create_only \
  --width 256 --height 256 --bands 1 --type Byte --threads 8 --iterations 1000000
```

## Matrix runner

There is also a helper script that runs a small predefined benchmark matrix and emits markdown tables that are easy to paste into an issue or PR:

```bash
python3 scripts/run_matrix.py --quick
```

You can restrict to selected scenarios or save raw JSON too:

```bash
python3 scripts/run_matrix.py --scenario single --scenario concurrent
python3 scripts/run_matrix.py --raw-json-dir results/
```

The current predefined scenarios are:

- `single`: `create_add_band`, `512x512x1 Byte`, `1` thread
- `concurrent`: `create_add_band`, `512x512x1 Byte`, `8` threads

One recent matrix run produced:

```markdown
## Direct MEM Advantage

| scenario | mem_cpp ops/s | vs driver_create | vs mem_open | vs mem_open_internal |
|---|---:|---:|---:|---:|
| single | 808,464.62 | 1.47x | 9.85x | 10.03x |
| concurrent | 2,174,564.29 | 4.99x | 24.27x | 23.51x |

## Full Comparison

| scenario | mode | ops/s | vs driver_create | mean ns/op | p95 ns | p99 ns | registered |
|---|---|---:|---:|---:|---:|---:|---|
| single | `mem_cpp` | 808,464.62 | 1.47x | 1,193.81 | 1,292 | 1,375 | no |
| single | `mem_driver_create` | 549,629.23 | 1.00x | 1,799.03 | 1,958 | 2,041 | yes |
| single | `mem_open` | 82,098.45 | 0.15x | 12,160.33 | 12,709 | 14,917 | yes |
| single | `mem_open_internal` | 80,572.15 | 0.15x | 12,389.76 | 13,041 | 18,750 | no |
| concurrent | `mem_cpp` | 2,174,564.29 | 4.99x | 3,603.04 | 11,750 | 34,208 | no |
| concurrent | `mem_driver_create` | 435,688.96 | 1.00x | 18,240.00 | 59,750 | 96,958 | yes |
| concurrent | `mem_open` | 89,589.55 | 0.21x | 88,688.52 | 183,083 | 253,209 | yes |
| concurrent | `mem_open_internal` | 92,501.53 | 0.21x | 86,192.71 | 178,250 | 240,625 | no |
```
