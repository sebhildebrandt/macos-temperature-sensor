#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  char name[128];   // service "Product" string
  double temp_c;    // Celsius
} mt_sensor_t;

typedef struct {
  double p_core_avg_c; // avg of "pACC MTR Temp Sensor*" sensors
  double e_core_avg_c; // avg of "eACC MTR Temp Sensor*" sensors
  size_t sensor_count; // number of returned sensors in sensors[]
  mt_sensor_t* sensors; // malloc'ed array; free with mt_free_snapshot()
} mt_snapshot_t;

// Collect a snapshot. Returns 0 on success, non-zero on error.
int mt_read_snapshot(mt_snapshot_t* out);

// Frees memory owned by snapshot (sensors array)
void mt_free_snapshot(mt_snapshot_t* s);

#ifdef __cplusplus
}
#endif
