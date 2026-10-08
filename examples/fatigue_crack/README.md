# fatigue_crack: a real in-situ fatigue experiment, streamed

Real beamline data through the connector: a crack starting at a corrosion pit
in an aluminium alloy and growing until the specimen breaks, replayed one
fatigue checkpoint per step. Two consumers follow it live, and neither one
receives the scans it is watching. Each subscribes to the part of the data it
needs, and the writer sends it only that part.

## The data

TomoBank's fatigue-corrosion series, `tomo_00032` to `tomo_00056` (Stannard
et al. 2017, DOI 10.17038/XSD/1373575). A corrosion-pitted, peak-aged Al7075
specimen was fatigue-tested in situ at APS beamline 2-BM-A, and was scanned by
micro-CT every few hundred to few thousand cycles, from 750 cycles to failure
at 14,346:

| | |
|---|---|
| Detector | PCO edge, 2560 x 2160, 0.65 um pixels, 27.4 keV |
| One scan | 1500 projections over 180 deg, plus 10 dark and 10 white frames |
| File | APS DXchange (HDF5), `/exchange/data` uint16, 16.8 GB |
| Series | 25 scans, ~420 GB |

The scan spacing tells the story: 14 scans cover the first 13,000 cycles, and
11 cover the last 1,346, because the crack speeds up near failure.

The files are on Globus only. Downloading them is free with an institutional
or ORCID login and Globus Connect Personal:
[TomoBank in-situ datasets](https://tomobank.readthedocs.io/en/latest/source/data/docs.data.insitu.html).
The scans do not record their fatigue cycle; that comes from the series' text
file, as `cycles.csv` (scan, fatigue_cycle) next to the scans.

## The programs

**`tomo_fatigue_writer`** plays the beamline. It reads each scan with plain
HDF5 and writes it through the connector, one step per checkpoint: the
DXchange datasets (one projection per `H5Dwrite()`, as a detector delivers
them), the angles, and `/measurement/sample/fatigue_cycle`. The beam energy
and pixel size are written once, in step 0. Every step rewrites the same
logical datasets, so the file is a DXchange file at every checkpoint and each
checkpoint is a re-openable revision of it.

**`crack_monitor.py --mode crack`** is the online-analysis node. It subscribes
to a band of detector rows (`--rows`, default 400:960) in every projection.
For each checkpoint it finds the rotation centre, reconstructs `--slices`
horizontal slices by filtered back-projection, and reports how much of each
cross-section is no longer metal. It needs only NumPy; with matplotlib it
also writes a PNG per checkpoint and `damage_vs_cycle.png`.

**`crack_monitor.py --mode preview`** is the control-room picture. It
subscribes to one projection plus one dark and one white frame.

## Running it

```bash
# BUILD_DIR built with the transport; DATA_DIR has tomo_*.h5 and cycles.csv
examples/fatigue_crack/run_fatigue_demo.sh BUILD_DIR DATA_DIR [OUT_DIR] [SCAN ...]
```

The script starts the writer, which waits for both subscribers before its
first step, then runs both consumers. It uses `na+sm` when
`kernel.yama.ptrace_scope` is 0 and `ofi+tcp` otherwise, because the
shared-memory bulk path needs cross-memory attach. It turns off the
`.payload` staging copy (`VOL_STREAM_STAGE_PAYLOAD=0`), which would otherwise
deflate a second copy of every 15.7 GiB step. Set `PYTHON` to an interpreter
with matplotlib for the plots.

## Measured: four checkpoints

`tomo_00032`, `00045`, `00050` and `00056` (750, 13,000, 13,800 and 14,346
cycles), on one workstation over `ofi+tcp`:

| Consumer | Received per checkpoint | Share of the 15.7 GiB written |
|---|---|---|
| crack monitor (rows 400:960) | 4,156 MiB | 25.93% |
| preview (1 projection + 2 flats) | 31.6 MiB | 0.20% |

Both are exactly the subscribed box (560 of 2160 rows; 3 of 1520 frames), and
every delivery was exact. The monitor's damaged fraction, worst slice per
checkpoint:

| Fatigue cycle | Worst slice | What it is |
|---|---|---|
| 750 | 0.013 | intact (the baseline: particles and pores) |
| 13,000 | 0.031 (row 404) | the crack entering from the pit, at the top of the band |
| 13,800 | 0.045 (row 561) | the crack, lower and wider; open voids in the slice |
| 14,346 | 0.980 (row 955) | the fracture gap: corrosion solution and gas bubbles |

The crack's row moves between checkpoints because the specimen itself moves.
These numbers equal those from reconstructing the same rows straight from
the scan files, so the band arrived intact.

Timing: each checkpoint took about 3.8 minutes, 85 s reading the scan, 3 s
capturing it, and 137 s in `H5Fend_step()` writing 15.7 GiB to the disk;
16 minutes in all. The writer peaked at 16.6 GB resident, since a step's
captured writes are held until `H5Fend_step()`. Reconstructing 8 slices took
the monitor 38 s per checkpoint, well inside the writer's cadence.

### Compression in transit, per subscriber

`crack_monitor.py --deflate N` has the writer deflate that subscriber's
images in transit (the run script passes `CRACK_DEFLATE` / `PREVIEW_DEFLATE`).
On the 750-cycle checkpoint, both at level 1:

| Consumer | Uncompressed | Deflated |
|---|---|---|
| crack monitor | 4,156.2 MiB | 2,908.5 MiB |
| preview | 31.6 MiB | 20.4 MiB |

The writer pushed 2,928.9 MiB in all (`VOL_STREAM_PUSH_STATS=1`), against
4,187.8 MiB uncompressed; the preview's share is its three frames deflated
offline with the same zlib level, and the band's is the rest. Lossless: the
monitor's damage numbers were identical. Noisy raw counts compress only about
1.4x, and compressing added 46 s to that checkpoint's `H5Fend_step()` (183 s
against 137 s). That run's writer also logged Mercury teardown errors at close
("HG core handles must be freed before destroying context"), with every exit
code 0 and the data correct. It did not recur in five repeats of the same
checkpoint (three deflated, two not), so it is intermittent: 1 of 4 deflated
runs, 0 of 3 uncompressed, and the one run that showed it also had
`VOL_STREAM_PUSH_STATS=1` set. The trigger is not established.

## Honest notes

- **The metric is a screen, not a fracture measurement.** "Damaged fraction"
  is the share of a slice's cross-section whose 7x7-smoothed attenuation is
  below half the alloy's (measured on the first checkpoint). It finds where
  the crack is and when it opens; it does not measure crack length, opening
  or growth rate, and it counts pores.
- **One band for every checkpoint.** The subscription is fixed, so the band
  is chosen wide enough to hold the crack as the specimen moves. A monitor
  that re-subscribed per checkpoint could follow it more tightly.
- **The reconstruction is plain filtered back-projection**, 2x binned, without
  ring removal or phase retrieval. The rings in the slices are the detector's.
- **The writer replays; it does not acquire.** Its timing is the disk's, not
  the beamline's: the real scans were minutes apart within hours of cycling.
- **The data are not in the repository** and there is no CI test, at 16.8 GB
  a scan.
