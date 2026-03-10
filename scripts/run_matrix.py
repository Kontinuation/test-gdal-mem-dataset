#!/usr/bin/env python3

import argparse
import json
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Scenario:
    name: str
    description: str
    operation: str
    width: int
    height: int
    bands: int
    data_type: str
    threads: int
    iterations: int
    warmup: int
    quick_iterations: int
    quick_warmup: int
    extra_args: tuple[str, ...] = ()


SCENARIOS = [
    Scenario(
        name="single",
        description="create_add_band, 256x256x1 Byte, 8 threads",
        operation="create_add_band",
        width=256,
        height=256,
        bands=1,
        data_type="Byte",
        threads=1,
        iterations=20000,
        warmup=1000,
        quick_iterations=2000,
        quick_warmup=200,
    ),
    Scenario(
        name="concurrent",
        description="create_add_band, 256x256x1 Byte, 8 threads",
        operation="create_add_band",
        width=256,
        height=256,
        bands=1,
        data_type="Byte",
        threads=8,
        iterations=20000,
        warmup=1000,
        quick_iterations=2000,
        quick_warmup=200,
    ),
]

MODE_ORDER = {
    "mem_cpp": 0,
    "mem_driver_create": 1,
    "mem_open": 2,
    "mem_open_internal": 3,
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run a predefined GDAL MEM benchmark matrix and emit markdown tables."
    )
    parser.add_argument(
        "--bench",
        default=str(Path(__file__).resolve().parent.parent / "gdal_mem_test"),
        help="Path to gdal_mem_test binary",
    )
    parser.add_argument(
        "--scenario",
        action="append",
        dest="scenarios",
        help="Scenario name to run. Repeatable. Defaults to all scenarios.",
    )
    parser.add_argument(
        "--quick",
        action="store_true",
        help="Use smaller iteration counts for a faster sweep.",
    )
    parser.add_argument(
        "--raw-json-dir",
        help="Optional directory to store raw JSON benchmark output per scenario.",
    )
    parser.add_argument(
        "--extra-bench-arg",
        action="append",
        default=[],
        help="Additional argument to pass through to gdal_mem_test. Repeatable.",
    )
    return parser.parse_args()


def selected_scenarios(names: list[str] | None) -> list[Scenario]:
    if not names:
        return list(SCENARIOS)
    known = {scenario.name: scenario for scenario in SCENARIOS}
    missing = [name for name in names if name not in known]
    if missing:
        raise SystemExit(f"Unknown scenario(s): {', '.join(missing)}")
    return [known[name] for name in names]


def benchmark_command(
    bench: str, scenario: Scenario, quick: bool, extra_args: list[str]
) -> list[str]:
    iterations = scenario.quick_iterations if quick else scenario.iterations
    warmup = scenario.quick_warmup if quick else scenario.warmup
    command = [
        bench,
        "--mode",
        "all",
        "--operation",
        scenario.operation,
        "--width",
        str(scenario.width),
        "--height",
        str(scenario.height),
        "--bands",
        str(scenario.bands),
        "--type",
        scenario.data_type,
        "--threads",
        str(scenario.threads),
        "--iterations",
        str(iterations),
        "--warmup",
        str(warmup),
        "--json",
    ]
    command.extend(scenario.extra_args)
    command.extend(extra_args)
    return command


def run_scenario(
    bench: str, scenario: Scenario, quick: bool, extra_args: list[str]
) -> list[dict]:
    command = benchmark_command(bench, scenario, quick, extra_args)
    sys.stderr.write(f"[run_matrix] running scenario {scenario.name}\n")
    sys.stderr.flush()
    print(" ".join(command))
    completed = subprocess.run(command, check=True, capture_output=True, text=True)
    try:
        return json.loads(completed.stdout)
    except json.JSONDecodeError as exc:
        raise RuntimeError(
            f"Failed to parse JSON output for scenario {scenario.name}: {exc}\n"
            f"stdout:\n{completed.stdout}\n"
            f"stderr:\n{completed.stderr}"
        ) from exc


def save_raw_json(
    raw_json_dir: str | None, scenario: Scenario, rows: list[dict]
) -> None:
    if not raw_json_dir:
        return
    out_dir = Path(raw_json_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    out_path = out_dir / f"{scenario.name}.json"
    out_path.write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")


def format_float(value: float) -> str:
    return f"{value:,.2f}"


def format_int(value: int) -> str:
    return f"{value:,}"


def speedup(numerator: float, denominator: float) -> str:
    if denominator <= 0.0:
        return "n/a"
    return f"{numerator / denominator:.2f}x"


def print_scenarios_section(scenarios: list[Scenario], quick: bool) -> None:
    print("## Scenario Definitions")
    print()
    for scenario in scenarios:
        iterations = scenario.quick_iterations if quick else scenario.iterations
        warmup = scenario.quick_warmup if quick else scenario.warmup
        print(
            f"- `{scenario.name}`: {scenario.description}; iterations/thread={iterations}, warmup/thread={warmup}"
        )
    print()


def print_direct_advantage_table(all_rows: dict[str, list[dict]]) -> None:
    print("## Direct MEM Advantage")
    print()
    print(
        "| scenario | mem_cpp ops/s | vs driver_create | vs mem_open | vs mem_open_internal |"
    )
    print("|---|---:|---:|---:|---:|")
    for scenario_name, rows in all_rows.items():
        by_mode = {row["mode"]: row for row in rows}
        mem_cpp = by_mode["mem_cpp"]["ops_per_s"]
        driver = by_mode["mem_driver_create"]["ops_per_s"]
        mem_open = by_mode["mem_open"]["ops_per_s"]
        mem_open_internal = by_mode["mem_open_internal"]["ops_per_s"]
        print(
            "| {scenario} | {mem_cpp} | {vs_driver} | {vs_open} | {vs_open_internal} |".format(
                scenario=scenario_name,
                mem_cpp=format_float(mem_cpp),
                vs_driver=speedup(mem_cpp, driver),
                vs_open=speedup(mem_cpp, mem_open),
                vs_open_internal=speedup(mem_cpp, mem_open_internal),
            )
        )
    print()


def print_full_comparison_table(all_rows: dict[str, list[dict]]) -> None:
    print("## Full Comparison")
    print()
    print(
        "| scenario | mode | ops/s | vs driver_create | mean ns/op | p95 ns | p99 ns | registered |"
    )
    print("|---|---|---:|---:|---:|---:|---:|---|")
    for scenario_name, rows in all_rows.items():
        by_mode = {row["mode"]: row for row in rows}
        driver = by_mode["mem_driver_create"]["ops_per_s"]
        for row in sorted(rows, key=lambda item: MODE_ORDER[item["mode"]]):
            print(
                "| {scenario} | `{mode}` | {ops} | {speedup_vs_driver} | {mean_ns} | {p95_ns} | {p99_ns} | {registered} |".format(
                    scenario=scenario_name,
                    mode=row["mode"],
                    ops=format_float(row["ops_per_s"]),
                    speedup_vs_driver=speedup(row["ops_per_s"], driver),
                    mean_ns=format_float(row["mean_ns_per_op"]),
                    p95_ns=format_int(int(row["p95_ns"])),
                    p99_ns=format_int(int(row["p99_ns"])),
                    registered="yes" if row["registered_in_global_open_list"] else "no",
                )
            )
    print()


def main() -> int:
    args = parse_args()
    bench = Path(args.bench).resolve()
    if not bench.exists():
        raise SystemExit(f"Benchmark binary not found: {bench}")

    scenarios = selected_scenarios(args.scenarios)
    all_rows: dict[str, list[dict]] = {}
    for scenario in scenarios:
        rows = run_scenario(str(bench), scenario, args.quick, args.extra_bench_arg)
        save_raw_json(args.raw_json_dir, scenario, rows)
        all_rows[scenario.name] = rows

    print(f"# GDAL MEM Benchmark Matrix ({'quick' if args.quick else 'default'})")
    print()
    print(f"- Benchmark binary: `{bench}`")
    print(f"- Scenarios run: {', '.join(scenario.name for scenario in scenarios)}")
    if args.quick:
        print(
            "- Note: quick mode uses reduced iteration counts and is intended for sanity checks, not final publishable numbers"
        )
    if args.raw_json_dir:
        print(f"- Raw JSON directory: `{Path(args.raw_json_dir).resolve()}`")
    print()

    print_scenarios_section(scenarios, args.quick)
    print_direct_advantage_table(all_rows)
    print_full_comparison_table(all_rows)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as exc:
        sys.stderr.write(exc.stdout)
        sys.stderr.write(exc.stderr)
        raise
