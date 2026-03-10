# Concurrent Profile Findings

This note summarizes profiling findings for the concurrent benchmark configuration used in the current matrix:

- shape: `512x512`, `1` band, `Byte`
- operation: `create_add_band`
- threads: `8`
- iterations per thread: `20000`
- profiler: macOS `/usr/bin/sample`

The relevant sample outputs are in `profiles/mem_cpp.sample.txt`, `profiles/mem_driver_create.sample.txt`, `profiles/mem_open.sample.txt`, and `profiles/mem_open_internal.sample.txt`.

## Benchmark results

One representative run with thread-local CPL options enabled:

| mode | ops/s | relative to `mem_cpp` |
|---|---:|---:|
| `mem_cpp` | 2,174,564.29 | 1.00x |
| `mem_driver_create` | 435,688.96 | 0.20x |
| `mem_open_internal` | 92,501.53 | 0.04x |
| `mem_open` | 89,589.55 | 0.04x |

`mem_open_internal` is only moderately faster than `mem_open`, so registration is not the main reason the open path is slow.

## Main findings

### 1. `mem_open` is mostly slow because it goes through the full GDAL open path

The dominant costs in `mem_open.sample.txt` and `mem_open_internal.sample.txt` are not `AddToDatasetOpenList()`.

The open path spends a lot of time in:

- `GDALDriverManager::GetDriver(int, bool)` with heavy mutex contention
- `GDALOpenInfo::GDALOpenInfo(...)`
- `VSIFOpenEx2L`
- `VSIUnixStdioFilesystemHandler::Open`
- `VSIStatExL`
- `GDALValidateOpenOptions(...)`
- `GDALMultiDomainMetadata::GetMetadataItem(...)`

This means the slowness comes primarily from generic open-time machinery:

- scanning registered drivers
- validating open options against driver metadata
- constructing `GDALOpenInfo`
- file-system style probing and stat/open attempts on the `MEM:::` string

The profiler supports this directly:

- `profiles/mem_open.sample.txt:35` shows time rooted in `GDALOpenEx`
- `profiles/mem_open.sample.txt:36` shows large time in `GDALDriverManager::GetDriver(int, bool)`
- `profiles/mem_open.sample.txt:77` shows large time in `GDALOpenInfo::GDALOpenInfo(...)`
- `profiles/mem_open.sample.txt:78` and `profiles/mem_open.sample.txt:79` show time in `VSIFOpenEx2L` and `VSIUnixStdioFilesystemHandler::Open`
- `profiles/mem_open.sample.txt:179` shows time in `VSIStatExL`
- `profiles/mem_open.sample.txt:1048` and `profiles/mem_open.sample.txt:1062` show `GDALValidateOpenOptions(...)`

The same pattern appears for the internal open mode:

- `profiles/mem_open_internal.sample.txt:35`
- `profiles/mem_open_internal.sample.txt:36`
- `profiles/mem_open_internal.sample.txt:80`
- `profiles/mem_open_internal.sample.txt:81`
- `profiles/mem_open_internal.sample.txt:152`
- `profiles/mem_open_internal.sample.txt:963`
- `profiles/mem_open_internal.sample.txt:2155`

Conclusion: the main problem with `mem_open` is indeed open-path overhead, especially driver lookup / probing and related locking, not dataset registration.

### 2. Registration overhead is real, but secondary for the `MEM:::` path

`mem_open` is slower than `mem_open_internal`, so the open-dataset list still costs something.

The profile for `mem_open` shows `GDALDataset::AddToDatasetOpenList()` and the associated mutex traffic:

- `profiles/mem_open.sample.txt:734`
- `profiles/mem_open.sample.txt:735`

But this is not the dominant stack. The bigger stacks are still `GetDriver`, `GDALOpenInfo`, `VSIFOpenEx2L`, `VSIStatExL`, and metadata validation.

So the difference between `mem_open` and `mem_open_internal` measures registration overhead, but the much larger gap between either open mode and `mem_cpp` is mostly generic open-path work.

### 3. `mem_driver_create` is slower than `mem_cpp` mainly because of registration and close-path bookkeeping

After changing the benchmark to cache the MEM driver handle once, `GDALGetDriverByName("MEM")` no longer shows up as a hot cost in `mem_driver_create`.

What remains is:

- `GDALDriver::Create(...)`
- `MEMDataset::Create(...)`
- `GDALDataset::AddToDatasetOpenList()`
- `GDALClose` / `GDALDataset::~GDALDataset()` cleanup

The sample clearly shows open-list insertion contention:

- `profiles/mem_driver_create.sample.txt:329`
- `profiles/mem_driver_create.sample.txt:330`
- `profiles/mem_driver_create.sample.txt:332`

And also close-path destructor overhead for registered datasets:

- `profiles/mem_driver_create.sample.txt:548`
- `profiles/mem_driver_create.sample.txt:551`

So once driver lookup is removed from the benchmark harness, `mem_driver_create` is mostly slower because create/destroy touches GDAL's global opened-dataset bookkeeping.

### 4. `mem_cpp` is the cleanest path, but it still pays some unrelated per-dataset setup cost

`mem_cpp` does not touch the driver manager or open-dataset list, but it still constructs a full `MEMDataset` object.

The main visible cost is inside `MEMDataset::Create(...)` and `MEMDataset::AddBand(...)`, especially initialization and object setup around band wiring.

- `profiles/mem_cpp.sample.txt:220`
- `profiles/mem_cpp.sample.txt:269`

The benchmark now sets frequently-requested config values as thread-local CPL options before each worker starts, which reduces contention from repeated config lookups. The remaining overhead is mostly object lifecycle work rather than config-option lookup.

So `mem_cpp` is not free, but it avoids both classes of extra work that hurt the alternatives:

- no driver-manager/open-path machinery
- no open-dataset registration on create/destroy

## Updated interpretation

The profiling result changes the emphasis of the upstream argument:

- For `GDALCreate(MEM, ...)`, the main extra cost is global dataset registration and teardown bookkeeping.
- For `GDALOpen("MEM:::")`, the main extra cost is the generic GDAL open path itself, especially driver lookup/probing, open-info setup, and option validation. Registration is only a smaller additional penalty.

So the case for a direct MEM C API is stronger than "avoid open-list registration": it would avoid two distinct sources of overhead depending on which public API callers use today.

## Practical implication for upstream proposal

A direct C API like `MEMCreate(...)` would provide a fast path with these properties:

- no driver-manager lookup/probing loop
- no `GDALOpenInfo` construction
- no `MEM:::` string parsing / file-system style probing
- no open-option validation path
- no insertion into the process-global open-dataset list

That is exactly why it should outperform both existing public alternatives in multi-threaded analytical workloads with many repeated temporary MEM dataset creations.
