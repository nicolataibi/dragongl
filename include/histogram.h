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
 * histogram.h - Histogram of durations with percentiles: the rounds of
 * the server (server_metrics.h) and the frames of the client.
 */

#ifndef DRAGONGL_HISTOGRAM_H
#define DRAGONGL_HISTOGRAM_H

#include <stdint.h>

/*Histogram of durations: METRICS_BUCKETS buckets of METRICS_BUCKET_US
 * microseconds, the last one open-ended. Percentiles are read at the upper
 * edge of their bucket: never below the truth, at most one bucket above.*/
#define METRICS_BUCKET_US 250
#define METRICS_BUCKETS   2000 /*0 .. 500 ms*/

typedef struct {
    uint64_t count;
    uint64_t sum_us;
    uint64_t max_us;
    uint64_t over_budget; /*samples longer than the budget of the series*/
    uint32_t bucket[METRICS_BUCKETS];
} MetricHistogram;

/*A reading of one histogram, in milliseconds.*/
typedef struct {
    uint64_t count;
    double mean_ms;
    double p50_ms;
    double p99_ms;
    double max_ms;
    uint64_t over_budget;
} MetricSummary;

void histogram_add(MetricHistogram *h, uint64_t usec, uint64_t budget_us);
/*The value under which pct percent of the samples fall (0 when empty).*/
double histogram_percentile_ms(const MetricHistogram *h, double pct);
void histogram_summary(const MetricHistogram *h, MetricSummary *out);

/*Monotonic clock in microseconds.*/
uint64_t metrics_now_us(void);

#endif
