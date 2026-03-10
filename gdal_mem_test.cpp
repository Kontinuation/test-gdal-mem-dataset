#include "cpl_conv.h"
#include "cpl_error.h"
#include "cpl_string.h"
#include "gdal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
#include <dlfcn.h>
#endif

namespace {

enum class Mode {
  kMemCpp,
  kMemDriverCreate,
  kMemOpen,
  kMemOpenInternal,
  kAll,
};

enum class Operation {
  kCreateOnly,
  kCreateAddBand,
  kCreateWritePixel,
  kCreateFull,
};

enum class Interleave {
  kBand,
  kPixel,
};

enum class WriteSize {
  k1x1,
  kScanline,
  kBlock,
};

struct Config {
  Mode mode = Mode::kMemCpp;
  Operation operation = Operation::kCreateOnly;
  Interleave interleave = Interleave::kBand;
  WriteSize write_size = WriteSize::k1x1;
  int width = 256;
  int height = 256;
  int bands = 1;
  int threads = 1;
  int iterations = 10000;
  int warmup = 1000;
  int repeat = 1;
  int block_width = 256;
  int block_height = 256;
  GDALDataType data_type = GDT_Byte;
  bool set_geo_transform = false;
  bool set_projection = false;
  bool json = false;
  bool csv = false;
  bool verbose = false;
};

struct CreationContext {
  GDALDatasetH dataset = nullptr;
};

struct ThreadFixture {
  std::vector<GByte> open_buffer;
  std::string open_string;
};

struct ThreadResult {
  std::vector<uint64_t> latencies_ns;
  double wall_seconds = 0.0;
};

struct SummaryStats {
  uint64_t min_ns = 0;
  uint64_t p50_ns = 0;
  uint64_t p95_ns = 0;
  uint64_t p99_ns = 0;
  uint64_t max_ns = 0;
  double mean_ns = 0.0;
};

struct BenchmarkResult {
  Mode mode;
  Operation operation;
  Interleave interleave;
  WriteSize write_size;
  int width = 0;
  int height = 0;
  int bands = 0;
  int threads = 0;
  int iterations = 0;
  int warmup = 0;
  int repeat_index = 0;
  int block_width = 0;
  int block_height = 0;
  GDALDataType data_type = GDT_Unknown;
  bool set_geo_transform = false;
  bool set_projection = false;
  bool expected_registered = false;
  uint64_t total_ops = 0;
  double total_wall_seconds = 0.0;
  double ops_per_second = 0.0;
  double fastest_thread_ops_per_second = 0.0;
  double slowest_thread_ops_per_second = 0.0;
  double imbalance_ratio = 0.0;
  SummaryStats latency;
};

using MemDatasetCreateFn = GDALDatasetH (*)(const char *, int, int, int,
                                            GDALDataType, char **);

constexpr const char *kProjectionWkt =
    "GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\",SPHEROID[\"WGS 84\","
    "6378137,298.257223563]],PRIMEM[\"Greenwich\",0],UNIT[\"degree\","
    "0.0174532925199433]]";

std::string mode_to_string(Mode mode) {
  switch (mode) {
  case Mode::kMemCpp:
    return "mem_cpp";
  case Mode::kMemDriverCreate:
    return "mem_driver_create";
  case Mode::kMemOpen:
    return "mem_open";
  case Mode::kMemOpenInternal:
    return "mem_open_internal";
  case Mode::kAll:
    return "all";
  }
  return "unknown";
}

std::string operation_to_string(Operation operation) {
  switch (operation) {
  case Operation::kCreateOnly:
    return "create_only";
  case Operation::kCreateAddBand:
    return "create_add_band";
  case Operation::kCreateWritePixel:
    return "create_write_pixel";
  case Operation::kCreateFull:
    return "create_full";
  }
  return "unknown";
}

std::string interleave_to_string(Interleave interleave) {
  switch (interleave) {
  case Interleave::kBand:
    return "band";
  case Interleave::kPixel:
    return "pixel";
  }
  return "unknown";
}

std::string write_size_to_string(WriteSize write_size) {
  switch (write_size) {
  case WriteSize::k1x1:
    return "1x1";
  case WriteSize::kScanline:
    return "scanline";
  case WriteSize::kBlock:
    return "block";
  }
  return "unknown";
}

void print_usage(const char *program_name) {
  std::cerr
      << "Usage: " << program_name << " [options]\n\n"
      << "Options:\n"
      << "  --mode <mem_cpp|mem_driver_create|mem_open|mem_open_internal|all>\n"
      << "  --operation <create_only|create_add_band|create_write_pixel|create_full>\n"
      << "  --width <int>\n"
      << "  --height <int>\n"
      << "  --bands <int>\n"
      << "  --type <GDAL type name>\n"
      << "  --threads <int>\n"
      << "  --iterations <int>\n"
      << "  --warmup <int>\n"
      << "  --repeat <int>\n"
      << "  --interleave <band|pixel>\n"
      << "  --set-geotransform\n"
      << "  --set-projection\n"
      << "  --write-size <1x1|scanline|block>\n"
      << "  --block-width <int>\n"
      << "  --block-height <int>\n"
      << "  --json\n"
      << "  --csv\n"
      << "  --verbose\n"
      << "  --list-modes\n"
      << "  --list-types\n"
      << "  --help\n";
}

[[noreturn]] void fail(const std::string &message) {
  throw std::runtime_error(message);
}

int parse_positive_int(const std::string &value, const std::string &flag_name,
                       bool allow_zero = false) {
  char *end = nullptr;
  errno = 0;
  const long parsed = std::strtol(value.c_str(), &end, 10);
  if (errno != 0 || end == nullptr || *end != '\0') {
    fail("Invalid integer for " + flag_name + ": " + value);
  }
  if (parsed < 0 || (!allow_zero && parsed == 0) ||
      parsed > std::numeric_limits<int>::max()) {
    fail("Out of range integer for " + flag_name + ": " + value);
  }
  return static_cast<int>(parsed);
}

Mode parse_mode(const std::string &value) {
  if (value == "mem_cpp") {
    return Mode::kMemCpp;
  }
  if (value == "mem_driver_create") {
    return Mode::kMemDriverCreate;
  }
  if (value == "mem_open") {
    return Mode::kMemOpen;
  }
  if (value == "mem_open_internal") {
    return Mode::kMemOpenInternal;
  }
  if (value == "all") {
    return Mode::kAll;
  }
  fail("Unknown mode: " + value);
}

Operation parse_operation(const std::string &value) {
  if (value == "create_only") {
    return Operation::kCreateOnly;
  }
  if (value == "create_add_band") {
    return Operation::kCreateAddBand;
  }
  if (value == "create_write_pixel") {
    return Operation::kCreateWritePixel;
  }
  if (value == "create_full") {
    return Operation::kCreateFull;
  }
  fail("Unknown operation: " + value);
}

Interleave parse_interleave(const std::string &value) {
  if (value == "band") {
    return Interleave::kBand;
  }
  if (value == "pixel") {
    return Interleave::kPixel;
  }
  fail("Unknown interleave: " + value);
}

WriteSize parse_write_size(const std::string &value) {
  if (value == "1x1") {
    return WriteSize::k1x1;
  }
  if (value == "scanline") {
    return WriteSize::kScanline;
  }
  if (value == "block") {
    return WriteSize::kBlock;
  }
  fail("Unknown write size: " + value);
}

GDALDataType parse_data_type(const std::string &value) {
  const GDALDataType type = GDALGetDataTypeByName(value.c_str());
  if (type == GDT_Unknown || type == GDT_TypeCount) {
    fail("Unknown GDAL data type: " + value);
  }
  return type;
}

bool expected_registered(Mode mode) {
  switch (mode) {
  case Mode::kMemCpp:
    return false;
  case Mode::kMemDriverCreate:
    return true;
  case Mode::kMemOpen:
    return true;
  case Mode::kMemOpenInternal:
    return false;
  case Mode::kAll:
    return false;
  }
  return false;
}

std::vector<Mode> expand_modes(Mode mode) {
  if (mode != Mode::kAll) {
    return {mode};
  }
  return {Mode::kMemCpp, Mode::kMemDriverCreate, Mode::kMemOpen,
          Mode::kMemOpenInternal};
}

void list_modes() {
  std::cout << "mem_cpp\n"
            << "mem_driver_create\n"
            << "mem_open\n"
            << "mem_open_internal\n"
            << "all\n";
}

void list_types() {
  for (int type = 1; type < static_cast<int>(GDT_TypeCount); ++type) {
    std::cout << GDALGetDataTypeName(static_cast<GDALDataType>(type)) << "\n";
  }
}

Config parse_args(int argc, char **argv) {
  Config cfg;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto require_value = [&](const std::string &flag) -> std::string {
      if (i + 1 >= argc) {
        fail("Missing value for " + flag);
      }
      return argv[++i];
    };

    if (arg == "--help") {
      print_usage(argv[0]);
      std::exit(0);
    }
    if (arg == "--list-modes") {
      list_modes();
      std::exit(0);
    }
    if (arg == "--list-types") {
      list_types();
      std::exit(0);
    }
    if (arg == "--mode") {
      cfg.mode = parse_mode(require_value(arg));
      continue;
    }
    if (arg == "--operation") {
      cfg.operation = parse_operation(require_value(arg));
      continue;
    }
    if (arg == "--width") {
      cfg.width = parse_positive_int(require_value(arg), arg);
      continue;
    }
    if (arg == "--height") {
      cfg.height = parse_positive_int(require_value(arg), arg);
      continue;
    }
    if (arg == "--bands") {
      cfg.bands = parse_positive_int(require_value(arg), arg, true);
      continue;
    }
    if (arg == "--type") {
      cfg.data_type = parse_data_type(require_value(arg));
      continue;
    }
    if (arg == "--threads") {
      cfg.threads = parse_positive_int(require_value(arg), arg);
      continue;
    }
    if (arg == "--iterations") {
      cfg.iterations = parse_positive_int(require_value(arg), arg);
      continue;
    }
    if (arg == "--warmup") {
      cfg.warmup = parse_positive_int(require_value(arg), arg, true);
      continue;
    }
    if (arg == "--repeat") {
      cfg.repeat = parse_positive_int(require_value(arg), arg);
      continue;
    }
    if (arg == "--interleave") {
      cfg.interleave = parse_interleave(require_value(arg));
      continue;
    }
    if (arg == "--set-geotransform") {
      cfg.set_geo_transform = true;
      continue;
    }
    if (arg == "--set-projection") {
      cfg.set_projection = true;
      continue;
    }
    if (arg == "--write-size") {
      cfg.write_size = parse_write_size(require_value(arg));
      continue;
    }
    if (arg == "--block-width") {
      cfg.block_width = parse_positive_int(require_value(arg), arg);
      continue;
    }
    if (arg == "--block-height") {
      cfg.block_height = parse_positive_int(require_value(arg), arg);
      continue;
    }
    if (arg == "--json") {
      cfg.json = true;
      continue;
    }
    if (arg == "--csv") {
      cfg.csv = true;
      continue;
    }
    if (arg == "--verbose") {
      cfg.verbose = true;
      continue;
    }
    fail("Unknown argument: " + arg);
  }

  if (cfg.json && cfg.csv) {
    fail("--json and --csv are mutually exclusive");
  }
  if (cfg.operation != Operation::kCreateAddBand && cfg.bands == 0) {
    fail("--bands must be > 0 for this operation");
  }
  if ((cfg.operation == Operation::kCreateWritePixel ||
       cfg.operation == Operation::kCreateFull) &&
      cfg.bands == 0) {
    fail("Write operations require --bands > 0");
  }
  return cfg;
}

size_t checked_multiply(size_t lhs, size_t rhs, const char *what) {
  if (lhs == 0 || rhs == 0) {
    return 0;
  }
  if (lhs > std::numeric_limits<size_t>::max() / rhs) {
    fail(std::string("Overflow computing ") + what);
  }
  return lhs * rhs;
}

size_t bytes_per_sample(GDALDataType data_type) {
  const int bytes = GDALGetDataTypeSizeBytes(data_type);
  if (bytes <= 0) {
    fail("Invalid data type size for " +
         std::string(GDALGetDataTypeName(data_type)));
  }
  return static_cast<size_t>(bytes);
}

size_t compute_open_buffer_size(int width, int height, int bands,
                                GDALDataType data_type,
                                Interleave interleave) {
  if (bands == 0) {
    return 0;
  }
  const size_t type_size = bytes_per_sample(data_type);
  const size_t width_u = static_cast<size_t>(width);
  const size_t height_u = static_cast<size_t>(height);
  const size_t bands_u = static_cast<size_t>(bands);
  if (interleave == Interleave::kBand) {
    return checked_multiply(
        checked_multiply(checked_multiply(width_u, height_u, "buffer size"),
                         bands_u, "buffer size"),
        type_size, "buffer size");
  }
  const size_t pixel_stride = checked_multiply(type_size, bands_u, "pixel stride");
  return checked_multiply(checked_multiply(width_u, height_u, "buffer size"),
                          pixel_stride, "buffer size");
}

ThreadFixture make_thread_fixture(const Config &cfg, Mode mode, int initial_bands,
                                  int thread_index) {
  ThreadFixture fixture;
  if (mode != Mode::kMemOpen && mode != Mode::kMemOpenInternal) {
    return fixture;
  }

  const size_t buffer_size =
      compute_open_buffer_size(cfg.width, cfg.height, initial_bands,
                               cfg.data_type, cfg.interleave);
  if (buffer_size > 0) {
    fixture.open_buffer.resize(buffer_size);
    std::fill(fixture.open_buffer.begin(), fixture.open_buffer.end(),
              static_cast<GByte>(thread_index & 0xff));
  }

  const size_t type_size = bytes_per_sample(cfg.data_type);
  size_t pixel_offset = type_size;
  size_t line_offset = checked_multiply(static_cast<size_t>(cfg.width), type_size,
                                        "line offset");
  size_t band_offset = checked_multiply(line_offset, static_cast<size_t>(cfg.height),
                                        "band offset");
  if (cfg.interleave == Interleave::kPixel && initial_bands > 0) {
    pixel_offset = checked_multiply(type_size, static_cast<size_t>(initial_bands),
                                    "pixel offset");
    line_offset = checked_multiply(pixel_offset, static_cast<size_t>(cfg.width),
                                   "line offset");
    band_offset = type_size;
  }

  std::ostringstream oss;
  const uintptr_t ptr_value = fixture.open_buffer.empty()
                                  ? static_cast<uintptr_t>(0)
                                  : reinterpret_cast<uintptr_t>(fixture.open_buffer.data());
  oss << "MEM:::DATAPOINTER=0x" << std::hex << std::uppercase << ptr_value
      << std::dec << ",PIXELS=" << cfg.width << ",LINES=" << cfg.height
      << ",BANDS=" << initial_bands << ",DATATYPE="
      << GDALGetDataTypeName(cfg.data_type) << ",PIXELOFFSET=" << pixel_offset
      << ",LINEOFFSET=" << line_offset << ",BANDOFFSET=" << band_offset;
  fixture.open_string = oss.str();
  return fixture;
}

MemDatasetCreateFn resolve_mem_dataset_create() {
#if defined(__APPLE__) || defined(__linux__)
  static std::once_flag once;
  static MemDatasetCreateFn resolved = nullptr;
  std::call_once(once, []() {
    const char *symbols[] = {
        "_ZN10MEMDataset6CreateEPKciii12GDALDataTypePPc",
        "?Create@MEMDataset@@SAPEAV1@PEBDHHHW4GDALDataType@@PEAPEAD@Z",
    };
    for (const char *symbol : symbols) {
      void *sym = dlsym(RTLD_DEFAULT, symbol);
      if (sym != nullptr) {
        resolved = reinterpret_cast<MemDatasetCreateFn>(sym);
        break;
      }
    }
  });
  return resolved;
#else
  return nullptr;
#endif
}

GDALDriverH resolve_mem_driver() {
  static std::once_flag once;
  static GDALDriverH driver = nullptr;
  std::call_once(once, []() { driver = GDALGetDriverByName("MEM"); });
  return driver;
}

char **make_interleave_options(Interleave interleave) {
  if (interleave == Interleave::kBand) {
    return nullptr;
  }
  char **options = nullptr;
  options = CSLSetNameValue(options, "INTERLEAVE", "PIXEL");
  return options;
}

CreationContext create_dataset(const Config &cfg, Mode mode,
                               const ThreadFixture &fixture, int initial_bands) {
  CreationContext ctx;
  if (mode == Mode::kMemCpp) {
    MemDatasetCreateFn create_fn = resolve_mem_dataset_create();
    if (create_fn == nullptr) {
      fail("Failed to resolve MEMDataset::Create symbol");
    }
    char **options = make_interleave_options(cfg.interleave);
    ctx.dataset = create_fn("", cfg.width, cfg.height, initial_bands,
                            cfg.data_type, options);
    CSLDestroy(options);
  } else if (mode == Mode::kMemDriverCreate) {
    GDALDriverH driver = resolve_mem_driver();
    if (driver == nullptr) {
      fail("Failed to resolve MEM driver");
    }
    char **options = make_interleave_options(cfg.interleave);
    ctx.dataset =
        GDALCreate(driver, "", cfg.width, cfg.height, initial_bands,
                   cfg.data_type, options);
    CSLDestroy(options);
  } else {
    const char *open_string = fixture.open_string.c_str();
    if (mode == Mode::kMemOpen) {
      ctx.dataset = GDALOpen(open_string, GA_Update);
    } else if (mode == Mode::kMemOpenInternal) {
      ctx.dataset = GDALOpenEx(open_string,
                               GDAL_OF_RASTER | GDAL_OF_UPDATE |
                                   GDAL_OF_INTERNAL,
                               nullptr, nullptr, nullptr);
    }
  }

  if (ctx.dataset == nullptr) {
    fail("Dataset creation failed for mode " + mode_to_string(mode));
  }
  return ctx;
}

void destroy_dataset(const CreationContext &ctx) {
  if (ctx.dataset != nullptr) {
    GDALClose(ctx.dataset);
  }
}

void add_bands(GDALDatasetH dataset, const Config &cfg) {
  for (int band_index = 0; band_index < cfg.bands; ++band_index) {
    if (GDALAddBand(dataset, cfg.data_type, nullptr) != CE_None) {
      fail("GDALAddBand failed");
    }
  }
}

void set_dataset_metadata(GDALDatasetH dataset, const Config &cfg) {
  if (cfg.set_geo_transform) {
    double transform[6] = {100.0, 0.5, 0.0, 200.0, 0.0, -0.5};
    if (GDALSetGeoTransform(dataset, transform) != CE_None) {
      fail("GDALSetGeoTransform failed");
    }
  }
  if (cfg.set_projection) {
    if (GDALSetProjection(dataset, kProjectionWkt) != CE_None) {
      fail("GDALSetProjection failed");
    }
  }
}

std::pair<int, int> write_window(const Config &cfg) {
  switch (cfg.write_size) {
  case WriteSize::k1x1:
    return {1, 1};
  case WriteSize::kScanline:
    return {cfg.width, 1};
  case WriteSize::kBlock:
    return {std::min(cfg.width, cfg.block_width),
            std::min(cfg.height, cfg.block_height)};
  }
  return {1, 1};
}

void write_dataset(GDALDatasetH dataset, const Config &cfg, int thread_index,
                   int iteration_index, bool full_write) {
  const auto [window_width, window_height] =
      full_write ? write_window(cfg) : std::make_pair(1, 1);
  const int value_count = window_width * window_height;
  std::vector<double> values(static_cast<size_t>(value_count));
  const double base_value = static_cast<double>(thread_index * 1000000 +
                                                iteration_index);
  for (int i = 0; i < value_count; ++i) {
    values[static_cast<size_t>(i)] = base_value + static_cast<double>(i);
  }
  const int band_count = GDALGetRasterCount(dataset);
  for (int band_index = 1; band_index <= band_count; ++band_index) {
    GDALRasterBandH band = GDALGetRasterBand(dataset, band_index);
    if (band == nullptr) {
      fail("Failed to fetch raster band");
    }
    if (GDALRasterIO(band, GF_Write, 0, 0, window_width, window_height,
                     values.data(), window_width, window_height, GDT_Float64,
                     0, 0) != CE_None) {
      fail("GDALRasterIO write failed");
    }
  }
}

void run_iteration(const Config &cfg, Mode mode, const ThreadFixture &fixture,
                   int thread_index, int iteration_index) {
  const int initial_bands =
      (cfg.operation == Operation::kCreateAddBand) ? 0 : cfg.bands;
  CreationContext ctx = create_dataset(cfg, mode, fixture, initial_bands);
  try {
    if (cfg.operation == Operation::kCreateAddBand) {
      add_bands(ctx.dataset, cfg);
    } else if (cfg.operation == Operation::kCreateWritePixel) {
      write_dataset(ctx.dataset, cfg, thread_index, iteration_index, false);
    } else if (cfg.operation == Operation::kCreateFull) {
      set_dataset_metadata(ctx.dataset, cfg);
      write_dataset(ctx.dataset, cfg, thread_index, iteration_index, true);
    }
  } catch (...) {
    destroy_dataset(ctx);
    throw;
  }
  destroy_dataset(ctx);
}

SummaryStats summarize_latencies(std::vector<uint64_t> latencies) {
  SummaryStats stats;
  if (latencies.empty()) {
    return stats;
  }
  std::sort(latencies.begin(), latencies.end());
  const auto get_percentile = [&](double p) -> uint64_t {
    const size_t index = static_cast<size_t>(
        std::ceil((p / 100.0) * static_cast<double>(latencies.size()))) -
                         1;
    return latencies[std::min(index, latencies.size() - 1)];
  };
  stats.min_ns = latencies.front();
  stats.p50_ns = get_percentile(50.0);
  stats.p95_ns = get_percentile(95.0);
  stats.p99_ns = get_percentile(99.0);
  stats.max_ns = latencies.back();
  const long double sum =
      std::accumulate(latencies.begin(), latencies.end(), 0.0L);
  stats.mean_ns = static_cast<double>(sum / latencies.size());
  return stats;
}

BenchmarkResult run_benchmark_once(const Config &cfg, Mode mode,
                                   int repeat_index) {
  enum Phase : int { kReady = 0, kWarmup = 1, kMeasure = 2 };

  const int initial_bands =
      (cfg.operation == Operation::kCreateAddBand) ? 0 : cfg.bands;
  std::vector<ThreadFixture> fixtures;
  fixtures.reserve(static_cast<size_t>(cfg.threads));
  for (int thread_index = 0; thread_index < cfg.threads; ++thread_index) {
    fixtures.push_back(make_thread_fixture(cfg, mode, initial_bands, thread_index));
  }

  std::vector<ThreadResult> thread_results(static_cast<size_t>(cfg.threads));
  std::vector<std::thread> threads;
  threads.reserve(static_cast<size_t>(cfg.threads));
  std::atomic<int> ready_count{0};
  std::atomic<int> warmup_done_count{0};
  std::atomic<int> phase{kReady};
  std::atomic<bool> failed{false};
  std::exception_ptr first_error;
  std::mutex error_mutex;

  for (int thread_index = 0; thread_index < cfg.threads; ++thread_index) {
    threads.emplace_back([&, thread_index]() {
      try {
        ThreadResult &result = thread_results[static_cast<size_t>(thread_index)];
        result.latencies_ns.reserve(static_cast<size_t>(cfg.iterations));

        ready_count.fetch_add(1, std::memory_order_acq_rel);
        while (phase.load(std::memory_order_acquire) < kWarmup) {
          std::this_thread::yield();
        }

        for (int i = 0; i < cfg.warmup; ++i) {
          run_iteration(cfg, mode, fixtures[static_cast<size_t>(thread_index)],
                        thread_index, -i - 1);
        }

        warmup_done_count.fetch_add(1, std::memory_order_acq_rel);
        while (phase.load(std::memory_order_acquire) < kMeasure) {
          std::this_thread::yield();
        }

        const auto thread_start = std::chrono::steady_clock::now();
        for (int i = 0; i < cfg.iterations; ++i) {
          if (failed.load(std::memory_order_acquire)) {
            return;
          }
          const auto op_start = std::chrono::steady_clock::now();
          run_iteration(cfg, mode, fixtures[static_cast<size_t>(thread_index)],
                        thread_index, i);
          const auto op_end = std::chrono::steady_clock::now();
          result.latencies_ns.push_back(static_cast<uint64_t>(
              std::chrono::duration_cast<std::chrono::nanoseconds>(op_end -
                                                                   op_start)
                  .count()));
        }
        const auto thread_end = std::chrono::steady_clock::now();
        result.wall_seconds =
            std::chrono::duration_cast<std::chrono::duration<double>>(thread_end -
                                                                      thread_start)
                .count();
      } catch (...) {
        failed.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> lock(error_mutex);
        if (!first_error) {
          first_error = std::current_exception();
        }
      }
    });
  }

  while (ready_count.load(std::memory_order_acquire) < cfg.threads) {
    std::this_thread::yield();
  }
  phase.store(kWarmup, std::memory_order_release);
  while (warmup_done_count.load(std::memory_order_acquire) < cfg.threads) {
    std::this_thread::yield();
  }
  const auto benchmark_start = std::chrono::steady_clock::now();
  phase.store(kMeasure, std::memory_order_release);

  for (auto &worker : threads) {
    worker.join();
  }
  const auto benchmark_end = std::chrono::steady_clock::now();

  if (first_error) {
    std::rethrow_exception(first_error);
  }

  std::vector<uint64_t> all_latencies;
  all_latencies.reserve(static_cast<size_t>(cfg.threads * cfg.iterations));
  double fastest_ops_per_second = 0.0;
  double slowest_ops_per_second = std::numeric_limits<double>::max();
  for (const ThreadResult &result : thread_results) {
    all_latencies.insert(all_latencies.end(), result.latencies_ns.begin(),
                         result.latencies_ns.end());
    const double thread_ops_per_second =
        result.wall_seconds > 0.0
            ? static_cast<double>(cfg.iterations) / result.wall_seconds
            : 0.0;
    fastest_ops_per_second =
        std::max(fastest_ops_per_second, thread_ops_per_second);
    slowest_ops_per_second =
        std::min(slowest_ops_per_second, thread_ops_per_second);
  }
  if (slowest_ops_per_second == std::numeric_limits<double>::max()) {
    slowest_ops_per_second = 0.0;
  }

  BenchmarkResult result;
  result.mode = mode;
  result.operation = cfg.operation;
  result.interleave = cfg.interleave;
  result.write_size = cfg.write_size;
  result.width = cfg.width;
  result.height = cfg.height;
  result.bands = cfg.bands;
  result.threads = cfg.threads;
  result.iterations = cfg.iterations;
  result.warmup = cfg.warmup;
  result.repeat_index = repeat_index;
  result.block_width = cfg.block_width;
  result.block_height = cfg.block_height;
  result.data_type = cfg.data_type;
  result.set_geo_transform = cfg.set_geo_transform;
  result.set_projection = cfg.set_projection;
  result.expected_registered = expected_registered(mode);
  result.total_ops = static_cast<uint64_t>(cfg.threads) *
                     static_cast<uint64_t>(cfg.iterations);
  result.total_wall_seconds =
      std::chrono::duration_cast<std::chrono::duration<double>>(benchmark_end -
                                                                benchmark_start)
          .count();
  result.ops_per_second =
      result.total_wall_seconds > 0.0
          ? static_cast<double>(result.total_ops) / result.total_wall_seconds
          : 0.0;
  result.fastest_thread_ops_per_second = fastest_ops_per_second;
  result.slowest_thread_ops_per_second = slowest_ops_per_second;
  result.imbalance_ratio =
      slowest_ops_per_second > 0.0
          ? fastest_ops_per_second / slowest_ops_per_second
          : 0.0;
  result.latency = summarize_latencies(std::move(all_latencies));
  return result;
}

std::string json_bool(bool value) { return value ? "true" : "false"; }

void print_result_human(const BenchmarkResult &result) {
  std::cout << "mode=" << mode_to_string(result.mode)
            << " operation=" << operation_to_string(result.operation)
            << " threads=" << result.threads
            << " iterations_per_thread=" << result.iterations
            << " warmup_per_thread=" << result.warmup
            << " repeat=" << result.repeat_index << "\n";
  std::cout << "shape=" << result.width << "x" << result.height
            << " bands=" << result.bands
            << " type=" << GDALGetDataTypeName(result.data_type)
            << " interleave=" << interleave_to_string(result.interleave)
            << " write_size=" << write_size_to_string(result.write_size)
            << " set_geotransform=" << (result.set_geo_transform ? "yes" : "no")
            << " set_projection=" << (result.set_projection ? "yes" : "no")
            << "\n";
  std::cout << std::fixed << std::setprecision(6)
            << "total_ops=" << result.total_ops
            << " total_time_s=" << result.total_wall_seconds
            << " throughput_ops_per_s=" << result.ops_per_second
            << " mean_ns_per_op=" << result.latency.mean_ns << "\n";
  std::cout << "latency_ns"
            << " min=" << result.latency.min_ns
            << " p50=" << result.latency.p50_ns
            << " p95=" << result.latency.p95_ns
            << " p99=" << result.latency.p99_ns
            << " max=" << result.latency.max_ns << "\n";
  std::cout << std::fixed << std::setprecision(2)
            << "per_thread_ops_per_s"
            << " fastest=" << result.fastest_thread_ops_per_second
            << " slowest=" << result.slowest_thread_ops_per_second
            << " imbalance_ratio=" << result.imbalance_ratio << "\n";
  std::cout << "registered_in_global_open_list="
            << (result.expected_registered ? "yes" : "no") << "\n\n";
}

void print_result_json(const std::vector<BenchmarkResult> &results) {
  std::cout << "[\n";
  for (size_t i = 0; i < results.size(); ++i) {
    const BenchmarkResult &result = results[i];
    std::cout << "  {"
              << "\"mode\":\"" << mode_to_string(result.mode) << "\","
              << "\"operation\":\"" << operation_to_string(result.operation)
              << "\","
              << "\"threads\":" << result.threads << ","
              << "\"iterations_per_thread\":" << result.iterations << ","
              << "\"warmup_per_thread\":" << result.warmup << ","
              << "\"repeat\":" << result.repeat_index << ","
              << "\"width\":" << result.width << ","
              << "\"height\":" << result.height << ","
              << "\"bands\":" << result.bands << ","
              << "\"data_type\":\"" << GDALGetDataTypeName(result.data_type)
              << "\","
              << "\"interleave\":\"" << interleave_to_string(result.interleave)
              << "\","
              << "\"write_size\":\"" << write_size_to_string(result.write_size)
              << "\","
              << "\"set_geotransform\":" << json_bool(result.set_geo_transform)
              << ","
              << "\"set_projection\":" << json_bool(result.set_projection)
              << ","
              << "\"registered_in_global_open_list\":"
              << json_bool(result.expected_registered) << ","
              << "\"total_ops\":" << result.total_ops << ","
              << std::fixed << std::setprecision(6)
              << "\"total_time_s\":" << result.total_wall_seconds << ","
              << "\"ops_per_s\":" << result.ops_per_second << ","
              << "\"mean_ns_per_op\":" << result.latency.mean_ns << ","
              << "\"min_ns\":" << result.latency.min_ns << ","
              << "\"p50_ns\":" << result.latency.p50_ns << ","
              << "\"p95_ns\":" << result.latency.p95_ns << ","
              << "\"p99_ns\":" << result.latency.p99_ns << ","
              << "\"max_ns\":" << result.latency.max_ns << ","
              << "\"fastest_thread_ops_per_s\":"
              << result.fastest_thread_ops_per_second << ","
              << "\"slowest_thread_ops_per_s\":"
              << result.slowest_thread_ops_per_second << ","
              << "\"imbalance_ratio\":" << result.imbalance_ratio << "}";
    if (i + 1 != results.size()) {
      std::cout << ",";
    }
    std::cout << "\n";
  }
  std::cout << "]\n";
}

void print_result_csv(const std::vector<BenchmarkResult> &results) {
  std::cout << "mode,operation,threads,iterations_per_thread,warmup_per_thread,repeat,width,height,bands,data_type,interleave,write_size,set_geotransform,set_projection,registered_in_global_open_list,total_ops,total_time_s,ops_per_s,mean_ns_per_op,min_ns,p50_ns,p95_ns,p99_ns,max_ns,fastest_thread_ops_per_s,slowest_thread_ops_per_s,imbalance_ratio\n";
  for (const BenchmarkResult &result : results) {
    std::cout << mode_to_string(result.mode) << ","
              << operation_to_string(result.operation) << "," << result.threads
              << "," << result.iterations << "," << result.warmup << ","
              << result.repeat_index << "," << result.width << ","
              << result.height << "," << result.bands << ","
              << GDALGetDataTypeName(result.data_type) << ","
              << interleave_to_string(result.interleave) << ","
              << write_size_to_string(result.write_size) << ","
              << (result.set_geo_transform ? "true" : "false") << ","
              << (result.set_projection ? "true" : "false") << ","
              << (result.expected_registered ? "true" : "false") << ","
              << result.total_ops << "," << std::fixed << std::setprecision(6)
              << result.total_wall_seconds << "," << result.ops_per_second
              << "," << result.latency.mean_ns << ","
              << result.latency.min_ns << "," << result.latency.p50_ns << ","
              << result.latency.p95_ns << "," << result.latency.p99_ns << ","
              << result.latency.max_ns << ","
              << result.fastest_thread_ops_per_second << ","
              << result.slowest_thread_ops_per_second << ","
              << result.imbalance_ratio << "\n";
  }
}

void print_environment(const Config &cfg) {
  if (cfg.json || cfg.csv) {
    return;
  }
  std::cout << "gdal_version=" << GDALVersionInfo("RELEASE_NAME") << "\n";
  std::cout << "gdal_mem_enable_open="
            << (CPLGetConfigOption("GDAL_MEM_ENABLE_OPEN", "") ?: "") << "\n";
  if (cfg.verbose) {
    std::cout << "gdal_build_info=" << GDALVersionInfo("BUILD_INFO") << "\n\n";
  }
}

} // namespace

int main(int argc, char **argv) {
  try {
    Config cfg = parse_args(argc, argv);

    GDALAllRegister();
    CPLSetConfigOption("GDAL_MEM_ENABLE_OPEN", "YES");

    print_environment(cfg);

    std::vector<BenchmarkResult> results;
    for (int repeat_index = 1; repeat_index <= cfg.repeat; ++repeat_index) {
      for (Mode mode : expand_modes(cfg.mode)) {
        results.push_back(run_benchmark_once(cfg, mode, repeat_index));
      }
    }

    if (cfg.json) {
      print_result_json(results);
    } else if (cfg.csv) {
      print_result_csv(results);
    } else {
      for (const BenchmarkResult &result : results) {
        print_result_human(result);
      }
    }

    GDALDestroyDriverManager();
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }
}
