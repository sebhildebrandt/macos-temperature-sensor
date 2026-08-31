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
  size_t sensor_count;
  mt_sensor_t* sensors;
} mt_snapshot_t;

typedef struct {
  char label[32];
  double rpm;
  double min;   // NaN if unknown
  double max;   // NaN if unknown
} mt_fan_t;

typedef struct {
  size_t fan_count;
  mt_fan_t* fans;
} mt_fans_t;

int mt_read_snapshot(mt_snapshot_t* out);

void mt_free_snapshot(mt_snapshot_t* s);

int mt_read_fans(mt_fans_t* out);

void mt_free_fans(mt_fans_t* s);

#ifdef __cplusplus
}
#endif
