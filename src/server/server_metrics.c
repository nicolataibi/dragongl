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
 * server_metrics.c - Duration histograms of the round and of the event
 * loop (see server_metrics.h).
 */

#include "server_metrics.h"

#include <string.h>
#include <time.h>

static MetricHistogram g_life[METRIC_SERIES];
static MetricHistogram g_window[METRIC_SERIES];
static uint64_t g_budget_us[METRIC_SERIES];

void metrics_init(uint64_t tick_budget_us, uint64_t loop_budget_us) {
    memset(g_life, 0, sizeof(g_life));
    memset(g_window, 0, sizeof(g_window));
    g_budget_us[METRIC_TICK] = tick_budget_us;
    g_budget_us[METRIC_LOOP] = loop_budget_us;
}

void metrics_record(MetricSeries s, uint64_t usec) {
    histogram_add(&g_life[s], usec, g_budget_us[s]);
    histogram_add(&g_window[s], usec, g_budget_us[s]);
}

void metrics_summary(MetricSeries s, bool lifetime, MetricSummary *out) {
    histogram_summary(lifetime ? &g_life[s] : &g_window[s], out);
}

void metrics_window_reset(void) {
    memset(g_window, 0, sizeof(g_window));
}
