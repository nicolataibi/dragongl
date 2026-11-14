/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * histogram.c - Duration histogram (histogram.h).
 */

#include "histogram.h"

#include <string.h>
#include <time.h>

void histogram_add(MetricHistogram *h, uint64_t usec, uint64_t budget_us) {
    uint64_t b = usec / METRICS_BUCKET_US;
    if (b >= METRICS_BUCKETS) {
        b = METRICS_BUCKETS - 1;
    }
    h->bucket[b]++;
    h->count++;
    h->sum_us += usec;
    if (usec > h->max_us) {
        h->max_us = usec;
    }
    if (budget_us > 0 && usec > budget_us) {
        h->over_budget++;
    }
}

double histogram_percentile_ms(const MetricHistogram *h, double pct) {
    if (h->count == 0) {
        return 0.0;
    }
    /*Rank of the sample: the smallest one whose cumulative share reaches
     * pct (nearest-rank).*/
    double exact = pct / 100.0 * (double)h->count;
    uint64_t rank = (uint64_t)exact;
    if ((double)rank < exact) {
        rank++;
    }
    if (rank < 1) {
        rank = 1;
    }
    uint64_t seen = 0;
    for (int b = 0; b < METRICS_BUCKETS; b++) {
        seen += h->bucket[b];
        if (seen >= rank) {
            if (b == METRICS_BUCKETS - 1) {
                return (double)h->max_us / 1000.0; /*open-ended bucket*/
            }
            uint64_t edge = (uint64_t)(b + 1) * METRICS_BUCKET_US;
            return (double)(edge < h->max_us ? edge : h->max_us) / 1000.0;
        }
    }
    return (double)h->max_us / 1000.0;
}

void histogram_summary(const MetricHistogram *h, MetricSummary *out) {
    memset(out, 0, sizeof(*out));
    out->count = h->count;
    out->over_budget = h->over_budget;
    if (h->count == 0) {
        return;
    }
    out->mean_ms = (double)h->sum_us / (double)h->count / 1000.0;
    out->p50_ms = histogram_percentile_ms(h, 50.0);
    out->p99_ms = histogram_percentile_ms(h, 99.0);
    out->max_ms = (double)h->max_us / 1000.0;
}

uint64_t metrics_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}
