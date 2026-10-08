/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * The beamline half of a real in-situ fatigue experiment, replayed.
 *
 * The data are TomoBank's fatigue-corrosion series (tomo_00032..00056):
 * micro-CT of a corrosion-pitted, peak-aged Al7075 specimen, scanned at APS
 * 2-BM-A at intervals of hundreds of fatigue cycles while a crack initiated
 * at a pit and grew to failure (Stannard et al. 2017, DOI
 * 10.17038/XSD/1373575). Each scan is an APS DXchange file: 1500 projections
 * of 2160x2560 uint16 over 180 degrees, plus 10 dark and 10 white frames.
 *
 * One step is one fatigue checkpoint. Inside a step this program does what
 * the beamline's own writer does -- it writes the DXchange datasets, one
 * projection per H5Dwrite(), as a detector delivers them -- and nothing
 * else. The fatigue cycle count, which the scan files themselves do not
 * carry, comes from cycles.csv and is written into the stream beside the
 * projections, so a consumer never needs a side channel to learn it.
 *
 * Because every step rewrites the same logical datasets, the file stays a
 * DXchange file at every checkpoint, and each checkpoint is a re-openable
 * revision of it. What consumers subscribe to is a sub-box of those same
 * datasets -- crack_monitor.py takes a band of rows around the crack, which
 * is the point of the demo: the writer sends each consumer only the part it
 * asked for.
 *
 * Usage: tomo_fatigue_writer OUT.h5 NCONSUMERS CYCLES.csv SCAN.h5 [SCAN.h5 ...]
 */

#include <errno.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define BARRIER_TIMEOUT_MS 600000
#define MAX_SCANS          64

#define DATA_PATH   "/exchange/data"
#define DARK_PATH   "/exchange/data_dark"
#define WHITE_PATH  "/exchange/data_white"
#define THETA_PATH  "/exchange/theta"
#define CYCLE_PATH  "/measurement/sample/fatigue_cycle"
#define ENERGY_PATH "/measurement/instrument/monochromator/energy"
#define PIXEL_PATH  "/measurement/instrument/detector/actual_pixel_size_x"

static double
now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* The fatigue cycle for SCAN's basename (minus .h5) from a "scan,fatigue_cycle" CSV. */
static long
lookup_cycle(const char *csv, const char *scan)
{
    char  stem[256], line[512], *b, *dot;
    char *copy = strdup(scan);
    FILE *f;
    long  cycle = -1;

    b = basename(copy);
    snprintf(stem, sizeof stem, "%s", b);
    free(copy);
    if ((dot = strstr(stem, ".h5")) != NULL)
        *dot = '\0';
    if ((f = fopen(csv, "r")) == NULL) {
        fprintf(stderr, "writer: FAIL open %s: %s\n", csv, strerror(errno));
        return -1;
    }
    while (fgets(line, sizeof line, f)) {
        char *comma = strchr(line, ',');
        if (comma == NULL)
            continue;
        *comma = '\0';
        if (strcmp(line, stem) == 0) {
            cycle = strtol(comma + 1, NULL, 10);
            break;
        }
    }
    fclose(f);
    if (cycle < 0)
        fprintf(stderr, "writer: FAIL %s is not in %s\n", stem, csv);
    return cycle;
}

/* Create a dataset like SRC's (type and the first NDIMS extents), one chunk per frame. */
static hid_t
create_like(hid_t fid, const char *path, hid_t type, int rank, const hsize_t *dims, hid_t lcpl)
{
    hid_t   space, dcpl, dset;
    hsize_t chunk[3];
    int     i;

    if ((space = H5Screate_simple(rank, dims, NULL)) < 0 || (dcpl = H5Pcreate(H5P_DATASET_CREATE)) < 0)
        return H5I_INVALID_HID;
    if (rank == 3) {
        /* One chunk per projection: the layout a detector writer produces,
         * and the one that makes a per-projection write exactly one chunk. */
        chunk[0] = 1;
        for (i = 1; i < 3; i++)
            chunk[i] = dims[i];
        if (H5Pset_chunk(dcpl, 3, chunk) < 0)
            return H5I_INVALID_HID;
    }
    dset = H5Dcreate2(fid, path, type, space, lcpl, dcpl, H5P_DEFAULT);
    H5Pclose(dcpl);
    H5Sclose(space);
    return dset;
}

/* Copy SRC's whole 3-D stack into DST, one frame per H5Dwrite(). */
static int
copy_frames(hid_t src, hid_t dst, const hsize_t *dims, uint16_t *buf, double *read_s)
{
    hid_t   sspace = H5Dget_space(src), dspace = H5Dget_space(dst), mspace;
    hsize_t start[3] = {0, 0, 0}, count[3] = {1, dims[1], dims[2]}, mdims[2] = {dims[1], dims[2]};
    hsize_t f;
    int     ret = 0;

    if (sspace < 0 || dspace < 0 || (mspace = H5Screate_simple(2, mdims, NULL)) < 0)
        return -1;
    for (f = 0; f < dims[0] && ret == 0; f++) {
        double t0 = now_s();

        start[0] = f;
        if (H5Sselect_hyperslab(sspace, H5S_SELECT_SET, start, NULL, count, NULL) < 0 ||
            H5Sselect_hyperslab(dspace, H5S_SELECT_SET, start, NULL, count, NULL) < 0 ||
            H5Dread(src, H5T_NATIVE_UINT16, mspace, sspace, H5P_DEFAULT, buf) < 0) {
            fprintf(stderr, "writer: FAIL read frame %llu\n", (unsigned long long)f);
            ret = -1;
            break;
        }
        *read_s += now_s() - t0;
        if (H5Dwrite(dst, H5T_NATIVE_UINT16, mspace, dspace, H5P_DEFAULT, buf) < 0) {
            fprintf(stderr, "writer: FAIL write frame %llu\n", (unsigned long long)f);
            ret = -1;
        }
    }
    H5Sclose(mspace);
    H5Sclose(dspace);
    H5Sclose(sspace);
    return ret;
}

static int
write_scalar(hid_t fid, const char *path, hid_t type, const void *value, hid_t lcpl, hid_t *keep)
{
    hid_t dset = *keep;

    if (dset == H5I_INVALID_HID) {
        hid_t space = H5Screate(H5S_SCALAR);
        dset        = H5Dcreate2(fid, path, type, space, lcpl, H5P_DEFAULT, H5P_DEFAULT);
        H5Sclose(space);
        if (dset < 0)
            return -1;
        *keep = dset;
    }
    return H5Dwrite(dset, type, H5S_ALL, H5S_ALL, H5P_DEFAULT, value) < 0 ? -1 : 0;
}

/* Read a scalar double from SRC if it exists; *out untouched otherwise. */
static void
read_scalar(hid_t src, const char *path, double *out)
{
    hid_t d;

    if (H5Lexists(src, "/measurement", H5P_DEFAULT) <= 0)
        return;
    H5E_BEGIN_TRY
    {
        if ((d = H5Dopen2(src, path, H5P_DEFAULT)) >= 0) {
            H5Dread(d, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, out);
            H5Dclose(d);
        }
    }
    H5E_END_TRY
}

int
main(int argc, char **argv)
{
    const char *out, *csv;
    int         nconsumers, nscans, s;
    long        cycles[MAX_SCANS];
    hid_t       vol_id, fapl, lcpl, fid;
    hid_t       data = H5I_INVALID_HID, dark = H5I_INVALID_HID, white = H5I_INVALID_HID;
    hid_t       theta = H5I_INVALID_HID, cycle_d = H5I_INVALID_HID, energy_d = H5I_INVALID_HID;
    hid_t       pixel_d = H5I_INVALID_HID;
    hsize_t     ddims[3] = {0, 0, 0}, fdims[3] = {0, 0, 0}, ntheta = 0;
    uint16_t   *buf = NULL;
    char        name[256];
    double      t_start = now_s();

    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc < 5) {
        fprintf(stderr, "usage: %s OUT.h5 NCONSUMERS CYCLES.csv SCAN.h5 [SCAN.h5 ...]\n", argv[0]);
        return 2;
    }
    out        = argv[1];
    nconsumers = atoi(argv[2]);
    csv        = argv[3];
    nscans     = argc - 4;
    if (nscans > MAX_SCANS) {
        fprintf(stderr, "writer: at most %d scans\n", MAX_SCANS);
        return 2;
    }
    for (s = 0; s < nscans; s++)
        if ((cycles[s] = lookup_cycle(csv, argv[4 + s])) < 0)
            return 1;

    setenv("VOL_STREAM_NA", "na+sm", 0);
    unlink(out);
    snprintf(name, sizeof name, "%s.vsgroup", out);
    unlink(name);
    snprintf(name, sizeof name, "%s.vsdone", out);
    unlink(name);

    if ((vol_id = H5VL_stream_register()) < 0 || (fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 ||
        H5Pset_vol(fapl, vol_id, NULL) < 0 || H5Pset_file_locking(fapl, false, true) < 0) {
        fprintf(stderr, "writer: FAIL register vol-stream / fapl\n");
        return 1;
    }
    if ((lcpl = H5Pcreate(H5P_LINK_CREATE)) < 0 || H5Pset_create_intermediate_group(lcpl, 1) < 0)
        return 1;
    if ((fid = H5Fcreate(out, H5F_ACC_TRUNC, H5P_DEFAULT, fapl)) < 0) {
        fprintf(stderr, "writer: FAIL create %s (transport up? VOL_STREAM_NA set?)\n", out);
        return 1;
    }

    printf("writer: %d fatigue checkpoint(s) -> %s\n", nscans, out);
    if (nconsumers > 0) {
        printf("writer: waiting up to %ds for %d consumer(s)...\n", BARRIER_TIMEOUT_MS / 1000, nconsumers);
        if (H5Fwait_subscribers(fid, (uint64_t)nconsumers, BARRIER_TIMEOUT_MS) < 0) {
            fprintf(stderr, "writer: FAIL only some of %d consumer(s) attached\n", nconsumers);
            return 1;
        }
        printf("writer: %d consumer(s) attached\n", nconsumers);
    }

    for (s = 0; s < nscans; s++) {
        const char *scan = argv[4 + s];
        hid_t       src, sdata, sdark, swhite, stheta, sp;
        hsize_t     d[3], wd[3], nt;
        double      read_s = 0.0, t0 = now_s(), t_end;
        double     *th;
        int64_t     cyc = (int64_t)cycles[s];

        /* The scans are ordinary HDF5: read them natively, not through the connector. */
        if ((src = H5Fopen(scan, H5F_ACC_RDONLY, H5P_DEFAULT)) < 0 ||
            (sdata = H5Dopen2(src, DATA_PATH, H5P_DEFAULT)) < 0 ||
            (sdark = H5Dopen2(src, DARK_PATH, H5P_DEFAULT)) < 0 ||
            (swhite = H5Dopen2(src, WHITE_PATH, H5P_DEFAULT)) < 0 ||
            (stheta = H5Dopen2(src, THETA_PATH, H5P_DEFAULT)) < 0) {
            fprintf(stderr, "writer: FAIL open DXchange datasets in %s\n", scan);
            return 1;
        }
        sp = H5Dget_space(sdata);
        H5Sget_simple_extent_dims(sp, d, NULL);
        H5Sclose(sp);
        sp = H5Dget_space(sdark);
        H5Sget_simple_extent_dims(sp, wd, NULL);
        H5Sclose(sp);
        sp = H5Dget_space(stheta);
        H5Sget_simple_extent_dims(sp, &nt, NULL);
        H5Sclose(sp);

        if (s == 0) {
            memcpy(ddims, d, sizeof d);
            memcpy(fdims, wd, sizeof wd);
            /* The APS files store one more angle than projections (1501 for
             * 1500); the stream keeps one angle per projection. */
            ntheta = nt < d[0] ? nt : d[0];
            if (NULL == (buf = (uint16_t *)malloc(d[1] * d[2] * sizeof(uint16_t)))) {
                fprintf(stderr, "writer: FAIL allocate frame\n");
                return 1;
            }
        }
        else if (memcmp(d, ddims, sizeof d) != 0 || memcmp(wd, fdims, sizeof wd) != 0) {
            fprintf(stderr, "writer: FAIL %s has a different shape from the first scan\n", scan);
            return 1;
        }

        if (H5Fbegin_step(fid, 0, NULL, 0) < 0) {
            fprintf(stderr, "writer: FAIL begin_step %d\n", s);
            return 1;
        }
        if (s == 0) {
            double energy = 0.0, pixel = 0.0;

            if ((data = create_like(fid, DATA_PATH, H5T_NATIVE_UINT16, 3, ddims, lcpl)) < 0 ||
                (dark = create_like(fid, DARK_PATH, H5T_NATIVE_UINT16, 3, fdims, lcpl)) < 0 ||
                (white = create_like(fid, WHITE_PATH, H5T_NATIVE_UINT16, 3, fdims, lcpl)) < 0 ||
                (theta = create_like(fid, THETA_PATH, H5T_NATIVE_DOUBLE, 1, &ntheta, lcpl)) < 0) {
                fprintf(stderr, "writer: FAIL create the DXchange datasets\n");
                return 1;
            }
            /* Instrument constants: written once, in step 0, on a different
             * cadence from the projections that follow every step. */
            read_scalar(src, ENERGY_PATH, &energy);
            read_scalar(src, PIXEL_PATH, &pixel);
            if (write_scalar(fid, ENERGY_PATH, H5T_NATIVE_DOUBLE, &energy, lcpl, &energy_d) < 0 ||
                write_scalar(fid, PIXEL_PATH, H5T_NATIVE_DOUBLE, &pixel, lcpl, &pixel_d) < 0) {
                fprintf(stderr, "writer: FAIL write instrument constants\n");
                return 1;
            }
        }

        if (write_scalar(fid, CYCLE_PATH, H5T_NATIVE_INT64, &cyc, lcpl, &cycle_d) < 0) {
            fprintf(stderr, "writer: FAIL write fatigue cycle\n");
            return 1;
        }
        if (NULL == (th = (double *)malloc(nt * sizeof(double))) ||
            H5Dread(stheta, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, th) < 0) {
            fprintf(stderr, "writer: FAIL read theta\n");
            return 1;
        }
        {
            hid_t   ms = H5Screate_simple(1, &ntheta, NULL);
            herr_t  st = H5Dwrite(theta, H5T_NATIVE_DOUBLE, ms, H5S_ALL, H5P_DEFAULT, th);
            H5Sclose(ms);
            free(th);
            if (st < 0) {
                fprintf(stderr, "writer: FAIL write theta\n");
                return 1;
            }
        }
        if (copy_frames(sdark, dark, fdims, buf, &read_s) < 0 ||
            copy_frames(swhite, white, fdims, buf, &read_s) < 0 ||
            copy_frames(sdata, data, ddims, buf, &read_s) < 0)
            return 1;

        t_end = now_s();
        if (H5Fend_step(fid) < 0) {
            fprintf(stderr, "writer: FAIL end_step %d\n", s);
            return 1;
        }
        printf("writer: step %d  %s  fatigue cycle %6ld  %llu projections  "
               "read %.1fs  capture %.1fs  end_step %.1fs  (%.1f GiB)\n",
               s, scan, cycles[s], (unsigned long long)ddims[0], read_s, t_end - t0 - read_s,
               now_s() - t_end,
               (double)(ddims[0] + 2 * fdims[0]) * (double)(ddims[1] * ddims[2] * 2) / (1u << 30));

        H5Dclose(stheta);
        H5Dclose(swhite);
        H5Dclose(sdark);
        H5Dclose(sdata);
        H5Fclose(src);
    }

    printf("writer: all checkpoints written in %.0fs, settling before close...\n", now_s() - t_start);
    usleep(1500000); /* outlive consumers still draining -- the settle window the other examples use */

    H5Dclose(data);
    H5Dclose(dark);
    H5Dclose(white);
    H5Dclose(theta);
    H5Dclose(cycle_d);
    H5Dclose(energy_d);
    H5Dclose(pixel_d);
    H5Pclose(lcpl);
    if (H5Fclose(fid) < 0) {
        fprintf(stderr, "writer: FAIL close %s\n", out);
        return 1;
    }
    H5Pclose(fapl);
    H5VLclose(vol_id);
    free(buf);
    printf("writer: done\n");
    return 0;
}
