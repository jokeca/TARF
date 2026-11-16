# TARF

TARF is a temperature-aware NAND refresh implementation built on MQSim. It estimates reliability from P/E cycles, programming/read temperatures, and retention age; selects migrations by expected risk reduction per execution cost; and schedules refresh with foreground I/O and garbage collection.

## Repository contents

| Directory | Contents |
|---|---|
| [`codes/`](codes/) | Simulator source, SSD configurations, and regression tests |
| [`data/`](data/chip_measurements.csv) | One CSV containing 152 chip measurement records |
| [`model_parameters/`](model_parameters/README.md) | Equation-to-code parameter mapping, fitted coefficient values, and the runtime model file |
| [`experiment_scripts/`](experiment_scripts/README.md) | Model fitting, policy comparison, scheduling ablation, and threshold-sensitivity scripts |

The measurement CSV contains 116 cross-temperature records and 36 retention records, using identifiers `Chip1` and `Chip2`. The primary model fits 96 default-read cross-temperature conditions and 16 Chip1 default-read retention conditions. The `used_in_main_fit` column identifies these records. Temperatures are in °C, retention age is in days, and `nfbc = fbc / 62`. Empty cells denote quantities that were not reported for that measurement family.

## Build and test

Requirements: Linux, GNU Make, a C++11 compiler, and Python 3.9 or later.

```bash
make
make test
```

The executable is `codes/MQSim`. Refresh uses simulated NAND reads, destination programs, mapping updates, and source erases. Destination P/E is preserved; retention age and programming temperature update when programming completes. Tests cover these operations, partial-block refresh, reserve handling, and measurement boundaries.

## Fit the model

```bash
python3 -m pip install -r requirements.txt
python3 experiment_scripts/fit_from_csv.py
```

This reads `data/chip_measurements.csv` and writes coefficients, fit statistics, and `model.conf` to `outputs/fit/`. The supplied parameter values are in `model_parameters/coefficients.csv`; the simulator loads `model_parameters/model.conf` directly.

## Run experiments

Provide a trace with five whitespace-separated fields: `timestamp device LBA sector_count operation`. LBAs and lengths use 512-byte sectors; `0` denotes write and `1` denotes read.

```bash
python3 experiment_scripts/run_experiments.py \
  --trace /path/to/requests.trace --source-unit ns \
  --study all --out outputs/comparison
```

The script runs five refresh policies, two scheduling ablations, and two additional trigger thresholds. Full TARF at threshold 10 serves as the shared reference. It uses three seeds by default and writes per-run measurements to `results.csv` in the output directory. Trace files are provided at run time.

See [experiment_scripts/README.md](experiment_scripts/README.md) for the run modes and initialization settings, and [model_parameters/README.md](model_parameters/README.md) for the formulas and coefficients.

## Main source files

| File | Function |
|---|---|
| `codes/src/ssd/Reliability.*` | Reliability model, programming history, and risk accounting |
| `codes/src/ssd/Refresh_Controller.*` | Candidate selection, refresh scheduling, and admission |
| `codes/src/ssd/GC_and_WL_Unit_*` | Physical migration and GC coordination |
| `codes/src/ssd/TSU_Priority_OutOfOrder.*` | Foreground/background NAND transaction scheduling |
| `codes/src/host/` | Request replay and response-time statistics |

Third-party licenses are recorded in [THIRD_PARTY.md](THIRD_PARTY.md).
