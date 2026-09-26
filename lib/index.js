const lib_version = require('../package.json').version;
const os = require('os');

const _platform = os.type();
const _darwin = _platform === 'Darwin';

// loaded lazily so the platform guard runs before the native require
let _native;
function native() {
  _native ??= require('../build/Release/mac_temp_native.node');
  return _native;
}

function version() {
  return lib_version;
}

function maxTemp(tempArray) {
  let max = -Infinity;
  for (const s of tempArray) {
    if (s > max) max = s;
  }
  return Number.isFinite(max) ? max : null;
}
function avgTemp(tempArray) {
  return tempArray.length
    ? tempArray.reduce((a, b) => a + b, 0) / tempArray.length
    : -1;
}

function temperature() {
  if (!_darwin) {
    throw new Error('CPU temperature reading not supported on this platform');
  }
  const tdie = []; // CPU
  const tpg = []; // SoC
  const tdev = []; // GPU (IOHID, up to M4)
  const tgSmc = []; // GPU (SMC, M5+)
  native().snapshot().sensors.forEach((element) => {
    const name = element.name.toLowerCase().replace(/^pmu2 /, 'pmu ');
    if (name.startsWith('pmu tdie')) {
      tdie.push(element.tempC);
    }
    if (name.startsWith('pmu tp') && name.endsWith('g')) {
      tpg.push(element.tempC);
    }
    if (name.startsWith('pmu tdev')) {
      tdev.push(element.tempC);
    }
    if (name.startsWith('smc tg')) {
      tgSmc.push(element.tempC);
    }
  });
  const gpuTemps = tdev.length ? tdev : tgSmc;
  return {
    cpu: maxTemp(tdie),
    soc: avgTemp(tdie.concat(tpg)),
    gpu: maxTemp(gpuTemps),
    cpuDieTemps: tdie,
    probeGroupsTemps: tpg,
    gpuDieTemps: gpuTemps,
  };
}

function fans() {
  if (!_darwin) {
    throw new Error('Fan reading not supported on this platform');
  }
  let raw = [];
  try {
    raw = native().fans();
  } catch {
    return [];
  }
  return raw.map((fan) => {
    // pwm is not exposed by the SMC, derived from the fan's min/max range
    const pwm =
      typeof fan.min === 'number' &&
      typeof fan.max === 'number' &&
      fan.max > fan.min
        ? Math.min(
            100,
            Math.max(0, ((fan.rpm - fan.min) / (fan.max - fan.min)) * 100),
          )
        : null;
    return {
      label: fan.label,
      rpm: fan.rpm,
      min: fan.min,
      max: fan.max,
      pwm,
    };
  });
}

exports.version = version;
exports.temperature = temperature;
exports.fans = fans;
