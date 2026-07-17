#include "temps.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void* IOHIDEventSystemClientRef;
typedef void* IOHIDServiceClientRef;
typedef void* IOHIDEventRef;

#ifndef kIOHIDEventTypeTemperature
#define kIOHIDEventTypeTemperature 15
#endif

static inline uint32_t IOHIDEventFieldBase_local(uint32_t type) {
  return (type << 16);
}

typedef IOHIDEventSystemClientRef (*pIOHIDEventSystemClientCreate)(CFAllocatorRef allocator);
typedef CFArrayRef (*pIOHIDEventSystemClientCopyServices)(IOHIDEventSystemClientRef client);

typedef CFTypeRef (*pIOHIDServiceClientCopyProperty)(IOHIDServiceClientRef service, CFStringRef key);
typedef IOHIDEventRef (*pIOHIDServiceClientCopyEvent)(IOHIDServiceClientRef service, int32_t type, uint64_t options, uint32_t timeout);

typedef double (*pIOHIDEventGetFloatValue)(IOHIDEventRef event, uint32_t field);

static pIOHIDEventSystemClientCreate      fClientCreate = NULL;
static pIOHIDEventSystemClientCopyServices fCopyServices = NULL;
static pIOHIDServiceClientCopyProperty     fCopyProperty = NULL;
static pIOHIDServiceClientCopyEvent        fCopyEvent = NULL;
static pIOHIDEventGetFloatValue            fGetFloatValue = NULL;

static int resolve_hid_symbols(void) {
  if (fClientCreate) return 1; // already resolved

  void* h = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_LAZY);
  if (!h) return 0;

  fClientCreate  = (pIOHIDEventSystemClientCreate)dlsym(h, "IOHIDEventSystemClientCreate");
  fCopyServices  = (pIOHIDEventSystemClientCopyServices)dlsym(h, "IOHIDEventSystemClientCopyServices");

  fCopyProperty  = (pIOHIDServiceClientCopyProperty)dlsym(h, "IOHIDServiceClientCopyProperty");
  fCopyEvent     = (pIOHIDServiceClientCopyEvent)dlsym(h, "IOHIDServiceClientCopyEvent");

  fGetFloatValue = (pIOHIDEventGetFloatValue)dlsym(h, "IOHIDEventGetFloatValue");

  return (fClientCreate && fCopyServices && fCopyProperty && fCopyEvent && fGetFloatValue) ? 1 : 0;
}

static int cfstring_to_cstr(CFStringRef s, char* out, size_t out_sz) {
  if (!s || !out || out_sz == 0) return 0;
  out[0] = '\0';
  return CFStringGetCString(s, out, (CFIndex)out_sz, kCFStringEncodingUTF8) ? 1 : 0;
}

static double read_temp_from_service(IOHIDServiceClientRef sc) {
  IOHIDEventRef ev = fCopyEvent(sc, kIOHIDEventTypeTemperature, 0, 0);
  if (!ev) return NAN;

  double v = fGetFloatValue(ev, IOHIDEventFieldBase_local(kIOHIDEventTypeTemperature));

  CFRelease((CFTypeRef)ev);
  return v;
}

#define SMC_CMD_READ_BYTES 5
#define SMC_CMD_READ_INDEX 8
#define SMC_CMD_READ_KEYINFO 9
#define SMC_KERNEL_INDEX 2
#define SMC_FOURCC(a, b, c, d) \
  (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))
#define SMC_MAX_GPU_KEYS 256

typedef struct {
  uint8_t major;
  uint8_t minor;
  uint8_t build;
  uint8_t reserved;
  uint16_t release;
} smc_vers_t;

typedef struct {
  uint16_t version;
  uint16_t length;
  uint32_t cpu_plimit;
  uint32_t gpu_plimit;
  uint32_t mem_plimit;
} smc_plimit_t;

typedef struct {
  uint32_t data_size;
  uint32_t data_type;
  uint8_t data_attributes;
} smc_keyinfo_t;

typedef struct {
  uint32_t key;
  smc_vers_t vers;
  smc_plimit_t plimit;
  smc_keyinfo_t keyinfo;
  uint8_t result;
  uint8_t status;
  uint8_t data8;
  uint32_t data32;
  uint8_t bytes[32];
} smc_keydata_t;

static io_connect_t smc_conn = 0;
static int smc_tried = 0;
static uint32_t smc_gpu_keys[SMC_MAX_GPU_KEYS];
static size_t smc_gpu_key_count = 0;

static int smc_call(smc_keydata_t* in, smc_keydata_t* out) {
  size_t out_size = sizeof(*out);
  kern_return_t kr = IOConnectCallStructMethod(smc_conn, SMC_KERNEL_INDEX, in, sizeof(*in), out, &out_size);
  return kr == KERN_SUCCESS && out->result == 0;
}

static double smc_read_flt(uint32_t key) {
  smc_keydata_t in, out;
  memset(&in, 0, sizeof(in));
  memset(&out, 0, sizeof(out));
  in.key = key;
  in.keyinfo.data_size = 4;
  in.data8 = SMC_CMD_READ_BYTES;
  if (!smc_call(&in, &out)) return NAN;
  float f;
  memcpy(&f, out.bytes, sizeof(f));
  return (double)f;
}

// discovers GPU die temperature keys ("Tg??", type flt) once; the set is fixed per boot
static void smc_scan_gpu_keys(void) {
  smc_keydata_t in, out;
  memset(&in, 0, sizeof(in));
  memset(&out, 0, sizeof(out));
  in.key = SMC_FOURCC('#', 'K', 'E', 'Y');
  in.keyinfo.data_size = 4;
  in.data8 = SMC_CMD_READ_BYTES;
  if (!smc_call(&in, &out)) return;
  uint32_t total = ((uint32_t)out.bytes[0] << 24) | ((uint32_t)out.bytes[1] << 16) |
                   ((uint32_t)out.bytes[2] << 8) | (uint32_t)out.bytes[3];

  for (uint32_t i = 0; i < total && smc_gpu_key_count < SMC_MAX_GPU_KEYS; i++) {
    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.data8 = SMC_CMD_READ_INDEX;
    in.data32 = i;
    if (!smc_call(&in, &out)) continue;
    uint32_t key = out.key;
    if (((key >> 24) & 0xff) != 'T' || ((key >> 16) & 0xff) != 'g') continue;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.key = key;
    in.data8 = SMC_CMD_READ_KEYINFO;
    if (!smc_call(&in, &out)) continue;
    if (out.keyinfo.data_type != SMC_FOURCC('f', 'l', 't', ' ') || out.keyinfo.data_size != 4) continue;

    smc_gpu_keys[smc_gpu_key_count++] = key;
  }
}

static int smc_open(void) {
  if (smc_conn) return 1;
  if (smc_tried) return 0;
  smc_tried = 1;

  io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AppleSMC"));
  if (!svc) return 0;

  kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &smc_conn);
  IOObjectRelease(svc);
  if (kr != KERN_SUCCESS) {
    smc_conn = 0;
    return 0;
  }

  smc_scan_gpu_keys();
  return 1;
}

int mt_read_snapshot(mt_snapshot_t* out) {
  if (!out) return 1;
  memset(out, 0, sizeof(*out));

  if (!resolve_hid_symbols()) {
    return 100;
  }

  IOHIDEventSystemClientRef client = fClientCreate(kCFAllocatorDefault);
  if (!client) return 2;

  CFArrayRef services = fCopyServices(client);
  if (!services) {
    CFRelease((CFTypeRef)client);
    return 3;
  }

  CFIndex n = CFArrayGetCount(services);
  if (n <= 0) {
    CFRelease(services);
    CFRelease((CFTypeRef)client);
    return 4;
  }

  size_t cap = 0;
  for (CFIndex i = 0; i < n; i++) {
    IOHIDServiceClientRef sc = (IOHIDServiceClientRef)CFArrayGetValueAtIndex(services, i);
    if (!sc) continue;
    CFTypeRef product = fCopyProperty(sc, CFSTR("Product"));
    if (product) { cap++; CFRelease(product); }
  }

  if (cap == 0) {
    CFRelease(services);
    CFRelease((CFTypeRef)client);
    return 5;
  }

  mt_sensor_t* sensors = (mt_sensor_t*)calloc(cap, sizeof(mt_sensor_t));
  if (!sensors) {
    CFRelease(services);
    CFRelease((CFTypeRef)client);
    return 6;
  }

  size_t used = 0;

  for (CFIndex i = 0; i < n; i++) {
    IOHIDServiceClientRef sc = (IOHIDServiceClientRef)CFArrayGetValueAtIndex(services, i);
    if (!sc) continue;

    CFTypeRef product = fCopyProperty(sc, CFSTR("Product"));
    if (!product) continue;
    if (CFGetTypeID(product) != CFStringGetTypeID()) { CFRelease(product); continue; }

    char name[128];
    if (!cfstring_to_cstr((CFStringRef)product, name, sizeof(name))) {
      CFRelease(product);
      continue;
    }
    CFRelease(product);

    double temp_c = read_temp_from_service(sc);

    // plausibility filter
    if (isnan(temp_c) || temp_c < -20.0 || temp_c > 130.0) continue;

    if (used < cap) {
      strncpy(sensors[used].name, name, sizeof(sensors[used].name) - 1);
      sensors[used].name[sizeof(sensors[used].name) - 1] = '\0';
      sensors[used].temp_c = temp_c;
      used++;
    }
  }

  if (smc_open() && smc_gpu_key_count > 0) {
    mt_sensor_t* grown = (mt_sensor_t*)realloc(sensors, (used + smc_gpu_key_count) * sizeof(mt_sensor_t));
    if (grown) {
      sensors = grown;
      for (size_t k = 0; k < smc_gpu_key_count; k++) {
        uint32_t key = smc_gpu_keys[k];
        double temp_c = smc_read_flt(key);
        if (isnan(temp_c) || temp_c <= 0.0 || temp_c > 130.0) continue;
        snprintf(sensors[used].name, sizeof(sensors[used].name), "SMC %c%c%c%c",
                 (int)((key >> 24) & 0xff), (int)((key >> 16) & 0xff),
                 (int)((key >> 8) & 0xff), (int)(key & 0xff));
        sensors[used].temp_c = temp_c;
        used++;
      }
    }
  }

  if (used == 0) {
    free(sensors);
    CFRelease(services);
    CFRelease((CFTypeRef)client);
    return 7;
  }

  mt_sensor_t* shrunk = (mt_sensor_t*)realloc(sensors, used * sizeof(mt_sensor_t));
  if (shrunk) sensors = shrunk;

  out->sensors = sensors;
  out->sensor_count = used;

  CFRelease(services);
  CFRelease((CFTypeRef)client);
  return 0;
}

void mt_free_snapshot(mt_snapshot_t* s) {
  if (!s) return;
  free(s->sensors);
  s->sensors = NULL;
  s->sensor_count = 0;
}
