#include "temps.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
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

static pthread_once_t hid_once = PTHREAD_ONCE_INIT;
static int hid_ok = 0;

static void hid_init(void) {
  void* h = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_LAZY);
  if (!h) return;

  fClientCreate  = (pIOHIDEventSystemClientCreate)dlsym(h, "IOHIDEventSystemClientCreate");
  fCopyServices  = (pIOHIDEventSystemClientCopyServices)dlsym(h, "IOHIDEventSystemClientCopyServices");

  fCopyProperty  = (pIOHIDServiceClientCopyProperty)dlsym(h, "IOHIDServiceClientCopyProperty");
  fCopyEvent     = (pIOHIDServiceClientCopyEvent)dlsym(h, "IOHIDServiceClientCopyEvent");

  fGetFloatValue = (pIOHIDEventGetFloatValue)dlsym(h, "IOHIDEventGetFloatValue");

  hid_ok = fClientCreate && fCopyServices && fCopyProperty && fCopyEvent && fGetFloatValue;
}

// partial resolution stays a failure; pthread_once guards concurrent first calls
static int resolve_hid_symbols(void) {
  pthread_once(&hid_once, hid_init);
  return hid_ok;
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
#define SMC_MAX_KEY_INDEX 16384  // sanity cap for #KEY
#define SMC_MAX_FANS 10  // F<n>* keys carry a single digit

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
static pthread_once_t smc_once = PTHREAD_ONCE_INIT;
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

// reads any numeric SMC key (flt / ui8 / ui16 / ui32); arm64 only, no fp2e/sp78 legacy formats
static int smc_read_num(uint32_t key, double* out) {
  smc_keydata_t in, o;
  memset(&in, 0, sizeof(in));
  memset(&o, 0, sizeof(o));
  in.key = key;
  in.data8 = SMC_CMD_READ_KEYINFO;
  if (!smc_call(&in, &o)) return 0;

  uint32_t type = o.keyinfo.data_type;
  uint32_t size = o.keyinfo.data_size;
  if (size == 0 || size > sizeof(o.bytes)) return 0;

  smc_keydata_t r;
  memset(&in, 0, sizeof(in));
  memset(&r, 0, sizeof(r));
  in.key = key;
  in.keyinfo.data_size = size;
  in.data8 = SMC_CMD_READ_BYTES;
  if (!smc_call(&in, &r)) return 0;

  if (type == SMC_FOURCC('f', 'l', 't', ' ') && size == 4) {
    float f;
    memcpy(&f, r.bytes, sizeof(f));
    *out = (double)f;
    return 1;
  }
  if (type == SMC_FOURCC('u', 'i', '8', ' ') || type == SMC_FOURCC('u', 'i', '1', '6') ||
      type == SMC_FOURCC('u', 'i', '3', '2')) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < size; i++) v = (v << 8) | r.bytes[i];
    *out = (double)v;
    return 1;
  }
  return 0;
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
  if (total > SMC_MAX_KEY_INDEX) total = SMC_MAX_KEY_INDEX;

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

// runs once per process; worker_threads share these globals
static void smc_init(void) {
  io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AppleSMC"));
  if (!svc) return;

  io_connect_t conn = 0;
  kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
  IOObjectRelease(svc);
  if (kr != KERN_SUCCESS) return;

  smc_conn = conn;
  smc_scan_gpu_keys();
}

static int smc_open(void) {
  pthread_once(&smc_once, smc_init);
  return smc_conn != 0;
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

int mt_read_fans(mt_fans_t* out) {
  if (!out) return 1;
  memset(out, 0, sizeof(*out));

  if (!smc_open()) return 2;

  double count = 0;
  if (!smc_read_num(SMC_FOURCC('F', 'N', 'u', 'm'), &count)) return 3;
  size_t n = (count > 0 && count <= SMC_MAX_FANS) ? (size_t)count : 0;
  if (n == 0) return 0;

  mt_fan_t* fans = (mt_fan_t*)calloc(n, sizeof(mt_fan_t));
  if (!fans) return 6;

  size_t used = 0;
  for (size_t i = 0; i < n; i++) {
    char d = (char)('0' + i);
    double rpm;
    if (!smc_read_num(SMC_FOURCC('F', d, 'A', 'c'), &rpm)) continue;
    if (isnan(rpm) || rpm < 0.0 || rpm > 20000.0) continue;

    double v;
    fans[used].min = smc_read_num(SMC_FOURCC('F', d, 'M', 'n'), &v) ? v : NAN;
    fans[used].max = smc_read_num(SMC_FOURCC('F', d, 'M', 'x'), &v) ? v : NAN;
    fans[used].rpm = rpm;
    snprintf(fans[used].label, sizeof(fans[used].label), "Fan %d", (int)(i + 1));
    used++;
  }

  if (used == 0) {
    free(fans);
    return 0;
  }

  out->fans = fans;
  out->fan_count = used;
  return 0;
}

void mt_free_fans(mt_fans_t* s) {
  if (!s) return;
  free(s->fans);
  s->fans = NULL;
  s->fan_count = 0;
}
