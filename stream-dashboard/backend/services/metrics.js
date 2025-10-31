const si = require('systeminformation');

/**
 * Collects system metrics (CPU, memory, load)
 */
class MetricsService {
  async getSystemMetrics() {
    try {
      const [cpu, mem, load] = await Promise.all([
        si.currentLoad(),
        si.mem(),
        si.currentLoad()
      ]);

      return {
        cpu: parseFloat(cpu.currentLoad.toFixed(2)),
        memory: parseFloat(((mem.used / mem.total) * 100).toFixed(2)),
        load: cpu.avgLoad ? [
          parseFloat(cpu.avgLoad.toFixed(2))
        ] : [0],
        memoryUsed: Math.round(mem.used / (1024 * 1024 * 1024) * 100) / 100, // GB
        memoryTotal: Math.round(mem.total / (1024 * 1024 * 1024) * 100) / 100 // GB
      };
    } catch (error) {
      console.error('[metrics] Error collecting system metrics:', error.message);
      return {
        cpu: 0,
        memory: 0,
        load: [0],
        memoryUsed: 0,
        memoryTotal: 0
      };
    }
  }
}

module.exports = new MetricsService();