/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * Helpers shared by the libFuzzer targets of tests/fuzz.
 *
 * Every target writes its own seed corpus from the current encoders when
 * DGL_FUZZ_SEEDS=<dir> is set (LLVMFuzzerInitialize then exits): the seeds
 * always match the formats of this tree, and no binary file is kept in
 * the repository.
 */

#ifndef DRAGONGL_FUZZ_COMMON_H
#define DRAGONGL_FUZZ_COMMON_H

#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerInitialize(int *argc, char ***argv);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/* The seed directory of DGL_FUZZ_SEEDS, or NULL when not set. */
const char *fuzz_seed_dir(void);

/* Write one seed file <dir>/<name>: a prefix byte (skipped if < 0)
 * followed by n bytes. Exits the process on an I/O error. */
void fuzz_write_seed(const char *dir, const char *name, int prefix, const void *data, size_t n);

/* A new private directory under $TMPDIR (default /tmp), created once
 * per process; exits on failure. */
const char *fuzz_tmp_dir(void);

#endif
