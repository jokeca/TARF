# Model parameters

`coefficients.csv` lists the numerical values and their equation/code names. `model.conf` is the runtime parameter file for the Chip1 configuration.

## Equation 3: temperature and wear

All logarithms are natural. Temperatures are in °C and P/E denotes the cycle count.

```text
d = abs(PT - RT) / 100
m = ((PT + RT) / 2 - 30) / 100
ln(NFBC_BOL) = beta_0 + beta_1 * ln(1 + PE) + beta_2 * d + beta_3 * m + b_c
```

| Symbol | Code name | Value |
|---|---|---:|
| β₀ | `beta0` | -3.1454924713304635 |
| β₁ | `beta_pe` | 0.28221072508724948 |
| β₂ | `beta_diff` | 2.0357945241726711 |
| β₃ | `beta_common` | -0.78029489966332577 |
| b₁ | `chip_bias`, Chip1 | 0 |
| b₂ | `chip_bias`, Chip2 | 0.45883869538447203 |

The temperature/wear fit pools 64 Chip1 and 32 Chip2 default-read conditions. Chip1 is the reference for the chip-specific intercept. `model.conf` uses `chip_bias=0`; the Chip2 intercept is included in the coefficient table.

## Equations 4–5: retention

```text
NFBC = NFBC_BOL * G(PE, t)
G(PE, t) = (1 + t / (t0 * (1 + PE)^(-delta)))^gamma
```

| Symbol | Code name | Chip1 value | Unit |
|---|---|---:|---|
| t₀ | `t0days` | 1.0161596365360437 | days |
| δ | `delta` | 0 | dimensionless |
| γ | `gamma` | 0.52542549511421621 | dimensionless |

Retention growth is normalized by the zero-age observation at each P/E level. The selected fit has δ = 0. Nominal ages of six months, one year, and three years correspond to 182.5, 365, and 1,095 days. Runtime aging uses 86,400 simulated seconds per day.

The included Chip2 retention records contain only zero-age conditions, so they do not identify Chip2 retention parameters. The runtime configuration uses the fitted Chip1 retention model. The temperature surface and retention factor are fitted separately and combined multiplicatively; retention below six months is an interpolation of the fitted function.

## Refit

```bash
python3 experiment_scripts/fit_from_csv.py --out outputs/fit
```

The output includes the same nine entries in `coefficients.csv`: four shared β coefficients, two chip intercepts, and three Chip1 retention parameters. `fit.json` also reports 14-fold validation with one unordered programming/read temperature pair held out per fold.

The refresh trigger and risk evaluation thresholds are policy settings, separate from the fitted coefficients.
