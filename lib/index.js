const lib_version = require('../package.json').version;
const m = require('../build/Release/mac_temp_native.node');
const os = require('os');

const _platform = os.type();
const _darwin = _platform === 'Darwin';

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
  m.snapshot().sensors.forEach((element) => {
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

exports.version = version;
exports.temperature = temperature;
