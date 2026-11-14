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
 * server_metrics.h - How long the server takes to do its work.
 *
 * The main loop records the duration of every round (update_world) and of
 * every pass of the event loop (messages served, round, flush). The
 * durations go into two histograms each: one for the life of the process
 * and one for the current reporting window, emptied by metrics_log().
 * Main thread only.
 */

#ifndef DRAGONGL_SERVER_METRICS_H
#define DRAGONGL_SERVER_METRICS_H

#include <stdbool.h>
#include <stdint.h>

/*What is measured.*/
typedef enum {
    METRIC_TICK, /*one round of the game: update_world()*/
    METRIC_LOOP, /*one pass of the event loop, the wait in epoll excluded*/
    METRIC_SERIES
} MetricSeries;

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

/*Budget of each series: a sample over it is counted as "over budget".*/
void metrics_init(uint64_t tick_budget_us, uint64_t loop_budget_us);
void metrics_record(MetricSeries s, uint64_t usec);
/*lifetime: since the start; otherwise the current window.*/
void metrics_summary(MetricSeries s, bool lifetime, MetricSummary *out);
/*Empties the windows.*/
void metrics_window_reset(void);

#endif
