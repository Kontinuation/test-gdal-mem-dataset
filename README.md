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

## Example benchmark matrix

```bash
./gdal_mem_test --mode all --operation create_only --width 1 --height 1 \
  --bands 1 --type Byte --threads 1 --iterations 200000 --warmup 5000

./gdal_mem_test --mode all --operation create_only --width 256 --height 256 \
  --bands 1 --type Byte --threads 8 --iterations 50000 --warmup 1000

./gdal_mem_test --mode all --operation create_add_band --width 256 --height 256 \
  --bands 4 --type Float32 --threads 8 --iterations 20000 --warmup 1000
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
python3 scripts/run_matrix.py --scenario tiny_single --scenario tiny_concurrent
python3 scripts/run_matrix.py --raw-json-dir results/
```
