const macTemp = require('../lib/index.js');

console.log('mac-temperature-sensor library version:', macTemp.version());

try {
  const snapshot = macTemp.temperature();
  console.log('Temperature snapshot:', snapshot);
} catch (error) {
  console.error('Error retrieving temperature:', error);
}

try {
  const fans = macTemp.fans();
  console.log('Fans:', fans);
} catch (error) {
  console.error('Error retrieving fans:', error);
}
