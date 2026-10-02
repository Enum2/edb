# Running the full benchmark suite on Windows WSL (clean environment)

This produces **one file — `RESULTS_FOR_MENTOR.txt`** — containing every result
(headline HTAP, B/C/D/E, concurrency+correctness, parameter sensitivity) plus
direct machine-state evidence (CPU log). Run it on WSL/Ubuntu, which has no
corporate endpoint-security file scanning, so the timings are clean.

## Steps

1. Copy this whole folder into WSL (e.g. `\\wsl$` or `cp -r`), then in a WSL shell:

   ```bash
   cd <this-folder>
   bash run_all_wsl.sh
   ```

   The script will: install dependencies (asks for sudo once), build, run the
   full suite with the warm-up protocol, log CPU throughout, and write
   `RESULTS_FOR_MENTOR.txt`.

2. (Optional) Quick 2-minute smoke test first, to confirm it builds and runs:

   ```bash
   SIZES="100000" HTAP_SIZES="100000" REPS=1 bash run_all_wsl.sh
   ```

3. Send `RESULTS_FOR_MENTOR.txt` to your mentor. The "MACHINE STATE DURING RUN"
   section should say **"security/indexing agents: NONE (clean environment)"** —
   that's the direct proof the numbers weren't distorted (unlike on the laptop).

## Notes

- Full run is heavy (hundreds of isolated runs, several hours at 5 reps). To go
  faster for a first pass, lower reps: `REPS=3 bash run_all_wsl.sh`.
- Dependencies (Ubuntu/Debian): `build-essential cmake liblmdb-dev
  librocksdb-dev libabsl-dev libsnappy-dev` — installed automatically.
- The RocksDB open path auto-adapts to the system RocksDB version (old Ubuntu
  22.04 ships 6.x, Ubuntu 24.04 ships 8.x — both build).
- All results also land in `clean_htap.csv`, `clean_bcde.csv`, `clean_exp4.csv`,
  `clean_param.csv`, and `cpu_samples.log` if you want the raw data.
