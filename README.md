# TARF

TARF is a temperature-aware NAND refresh implementation built on MQSim. It uses retention age, P/E cycles, and programming/read temperatures to estimate data reliability. It selects refresh operations by their expected reliability benefit and migration cost, schedules them around foreground I/O, and coordinates them with garbage collection.

## Implementation

- **Reliability model:** tracks programming history and retention age, then evaluates wear and temperature effects for live data.
- **Refresh selection:** estimates the benefit of moving data to an available destination block and ranks candidates by benefit per estimated execution cost.
- **Scheduling:** considers channel/chip availability and queued foreground requests, with a bounded admission deferral for background progress.
- **GC coordination:** shares the physical migration path and uses reliability benefit to break ties between equally reclaimable GC victims.

Refresh performs NAND reads, destination programs, mapping updates, and source erases. The destination retains its actual P/E count. Programming temperature and retention age update when programming completes. Retention uses days, with 86,400 simulated seconds per day.

## Code layout

| Path | Purpose |
|---|---|
| `code_tarf/src/ssd/Reliability.*` | Reliability model, programming history, and risk accounting |
| `code_tarf/src/ssd/Refresh_Controller.*` | Refresh policies, candidate ranking, and resource admission |
| `code_tarf/src/ssd/GC_and_WL_Unit_*` | NAND migration and GC coordination |
| `code_tarf/src/ssd/TSU_Priority_OutOfOrder.*` | Foreground/background NAND transaction scheduling |
| `code_tarf/src/host/` | Request replay and response-time accounting |
| `configs/ssd.xml` | SSD configuration |
| `configs/model.conf` | Fitted reliability-model parameters |
| `scripts/` | Device-workbook calibration and retention fitting |
| `tests/`, `code_tarf/tests/` | Simulator regression tests |

## Build and test

Requirements: Linux, GNU Make, a C++11 compiler, and Python 3.9 or later.

```bash
make
make test
```

The executable is `code_tarf/MQSim`. Tests cover reliability calculations, metadata updates, real NAND refresh operations, partial-block migration, free-space reserves, and measurement boundaries.

## Run

Supply a trace-flow XML file through MQSim's `-w` option:

```bash
TARF_POLICY=tarf \
TARF_MODEL_FILE="$PWD/configs/model.conf" \
TARF_OUTPUT_PREFIX="$PWD/outputs/run" \
./code_tarf/MQSim -i configs/ssd.xml -w /path/to/io.xml
```

| Environment variable | Function |
|---|---|
| `TARF_POLICY` | `none`, `periodic`, `age`, `tempdiff`, or `tarf` |
| `TARF_THRESHOLD` | Refresh-trigger threshold |
| `TARF_RISK_THRESHOLD` | Common threshold used for risk accounting |
| `TARF_PERIOD_DAYS` | Retention-age interval for periodic refresh |
| `TARF_RESOURCE_AWARE` | Enable/disable resource-aware admission (`1`/`0`) |
| `TARF_GC_COORDINATION` | Enable/disable reliability-aware GC coordination (`1`/`0`) |
| `TARF_HORIZON_NS`, `TARF_SCAN_NS` | Simulation duration and scan interval in nanoseconds |
| `TARF_INITIAL_STATE_FILE` | Initial block P/E, retention age, and programming temperature |
| `TARF_TEMPERATURE_FILE` | Operating-temperature trajectory |

`none` performs normal GC without proactive refresh. `periodic` uses retention age; `age` uses retention age and wear; `tempdiff` adds temperature mismatch; `tarf` uses the full model and scheduling mechanisms.

Each run writes host response statistics, reliability counters, migration events, and threshold-wait records under the output prefix. Risk accounting includes unresolved threshold episodes and unfinished refresh work at the simulation endpoint.

## Model fitting

The included model parameters can be loaded directly. To refit from a device measurement workbook:

```bash
python3 -m pip install -r requirements.txt
python3 scripts/calibrate_micron.py --workbook /path/to/device_measurements.xlsx --out outputs/calibration
```

The fitter uses log-NFBC regression for temperature/wear conditions and fits retention growth after normalization by the zero-age observation at each P/E level. Runtime risk exposure is derived from the fitted model.

See [THIRD_PARTY.md](THIRD_PARTY.md) for the MQSim and RapidXML licenses.
