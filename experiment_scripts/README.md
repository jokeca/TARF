# Experiment scripts

| Script | Purpose |
|---|---|
| `run_experiments.py` | Prepare shared inputs, execute comparisons, and collect measured results |
| `fit_from_csv.py` | Refit the model from the included measurement CSV |
| `calibrate_micron.py` | Extract and fit measurements from a local device workbook |
| `fit_retention.py` | Fit retention curves from condition records |

## Run modes

```bash
python3 experiment_scripts/run_experiments.py \
  --trace /path/to/requests.trace --source-unit ns \
  --study all --out outputs/comparison
```

| `--study` | Configurations per seed |
|---|---|
| `main` | No Refresh, Periodic, Age-based, TempDiff, TARF |
| `ablation` | TARF, TARF without resource-aware scheduling, TARF without GC coordination |
| `threshold` | TARF trigger thresholds 8, 10, and 12 |
| `all` | Nine unique configurations; full TARF at threshold 10 is reused |

The default seeds are 321, 2027, and 4099. Use `--seeds 321` to select one seed. `--prepare-only` writes shared inputs and `manifest.json` without launching simulations. Each invocation uses a new output directory. Runs execute sequentially.

## Shared setup

The default SSD configuration is `codes/ssd.xml`: 512 decimal GB of user capacity, 528 GiB of raw NAND, and 16 KiB pages. `--ssd` selects another configuration. The default setup uses:

- 90% logical prefill, plus pages required for reads that precede their first write.
- Initial P/E values 300, 1,000, and 3,000 with probabilities 40%, 30%, and 30%.
- Initial retention age zero with probability 70%; otherwise uniform over 182.5–1,095 days.
- Initial programming temperatures uniform over 0–85 °C; operating temperature 55 °C.
- A complete warm-up replay, 600 s settling, a complete measured replay, and 1,800 s drain.
- A 10 s scan interval, trigger threshold 10, and fixed risk evaluation threshold 10.
- One in-flight refresh page per chip for TARF and two for the other refreshing policies; 100 ms admission deferral and 600 s TARF reserve escape interval.

All policies for a seed share the request stream, initial state, temperature, and SSD configuration. Proactive refresh starts at the measurement boundary. The collector compares boundary-state hashes across policies. The no-scheduling ablation changes `TARF_RESOURCE_AWARE`; the no-GC-coordination ablation changes `TARF_GC_COORDINATION`.

## Inputs and controls

The trace has five whitespace-separated integer fields:

```text
timestamp device start_LBA sector_count operation
```

LBAs and lengths use 512-byte sectors. Operation `0` means write and `1` means read. Every request is retained. The script subtracts the first timestamp and converts the declared source unit to nanoseconds. `--timestamp-divisor` explicitly compresses arrival timestamps using integer division; the default is 1. This transformation does not scale retention aging.

| Option | Purpose |
|---|---|
| `--source-unit ns`, `100ns`, or `us` | Timestamp unit in the input trace |
| `--duration-seconds` | Explicit duration of one replay; default ends at the next whole second after the final request |
| `--initial-state` | CSV with `block_index,pe,age_days,program_temp_c` |
| `--temperature-file` | CSV with `time_ns,temp_C` |
| `--model` | Reliability-model parameter file |
| `--occupancy` | Initial logical prefill percentage |
| `--settle-seconds`, `--drain-seconds` | Explicit settling and drain durations |
| `--timeout` | Wall-clock timeout per simulator process; default 21,600 s |

For replay duration D, the simulation lasts `2D + settling + drain + 1 ns`. The 1 ns guard places the measured replay after the measurement snapshot.

## Outputs

Each run produces host statistics, reliability counters, migration events, and threshold-wait episodes. `results.csv` reports completed refresh and ordinary-GC migration bytes, mean/P99 response time, NFBC exposure, request/byte completion, maximum observed threshold wait, and end backlogs. Latency uses the measured arrival cohort; migration and risk subtract the warm-up boundary counters and include the drain.

Runs with unfinished foreground requests or overlapping warm-up work have status `incomplete`. When the selected study includes TempDiff, the script also evaluates TARF against the same-seed reference: at most 1.05 times its exposure and risky-block seconds, at most 1.05 times its maximum wait plus 10 s, and no larger ending backlog. `accepted` additionally requires all requests/bytes completed, a drained warm-up, and zero TARF end backlog. This paired check is left empty when the study does not run TempDiff.

## Model fitting

```bash
python3 -m pip install -r requirements.txt
python3 experiment_scripts/fit_from_csv.py
```

For a local copy of the device workbook:

```bash
python3 experiment_scripts/calibrate_micron.py \
  --workbook /path/to/device_measurements.xlsx --out outputs/calibration
```
