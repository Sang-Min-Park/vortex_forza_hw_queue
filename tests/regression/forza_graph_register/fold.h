#pragma once

#include "common.h"

struct fold_stats_t {
  uint64_t edges;
  uint64_t flushes;
};

static inline void flush_value(int64_t* target, uint32_t key, int64_t sum) {
  int64_t* address = target + key;
  __asm__ volatile ("amoadd.d zero, %1, (%0)" :: "r"(address), "r"(sum) : "memory");
}

static __attribute__((noinline)) fold_stats_t fold_range(
    const uint32_t* rows, const graph_edge_t* edges, const int64_t* x,
    int64_t* target, uint32_t vertices, uint32_t edge_begin,
    uint32_t edge_end, uint32_t edge_step) {
  fold_stats_t stats{};
  if (edge_begin < edge_end) {
    uint32_t low = 0, high = vertices;
    while (low + 1 < high) {
      uint32_t middle = low + (high - low) / 2;
      if (rows[middle] <= edge_begin) low = middle;
      else high = middle;
    }
    uint32_t source = low;
    uint32_t source_end = rows[source + 1];
    int64_t source_value = x[source];
#ifndef GRAPH_REGISTER_RANGE
    uint32_t key0 = UINT32_MAX, key1 = UINT32_MAX;
    int64_t sum0 = 0, sum1 = 0;
    bool recent0 = false;
#endif
    for (uint32_t edge = edge_begin; edge < edge_end; edge += edge_step) {
      if (edge >= source_end) {
        do {
          ++source;
          source_end = rows[source + 1];
        } while (edge >= source_end);
        source_value = x[source];
      }
      graph_edge_t entry = edges[edge];
      int64_t contribution = source_value * entry.weight;
#ifdef GRAPH_REGISTER_RANGE
      flush_value(target, entry.destination, contribution);
      ++stats.flushes;
#else
      if (entry.destination == key0) {
        sum0 += contribution;
        recent0 = true;
      } else if (entry.destination == key1) {
        sum1 += contribution;
        recent0 = false;
      } else if (key0 == UINT32_MAX) {
        key0 = entry.destination;
        sum0 = contribution;
        recent0 = true;
      } else if (key1 == UINT32_MAX) {
        key1 = entry.destination;
        sum1 = contribution;
        recent0 = false;
      } else if (recent0) {
        flush_value(target, key1, sum1);
        ++stats.flushes;
        key1 = entry.destination;
        sum1 = contribution;
        recent0 = false;
      } else {
        flush_value(target, key0, sum0);
        ++stats.flushes;
        key0 = entry.destination;
        sum0 = contribution;
        recent0 = true;
      }
#endif
      ++stats.edges;
    }
#ifndef GRAPH_REGISTER_RANGE
    if (key0 != UINT32_MAX) {
      flush_value(target, key0, sum0);
      ++stats.flushes;
    }
    if (key1 != UINT32_MAX) {
      flush_value(target, key1, sum1);
      ++stats.flushes;
    }
#endif
  }
  return stats;
}
