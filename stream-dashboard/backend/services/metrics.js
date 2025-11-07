const si = require('systeminformation');
const { exec } = require('child_process');
const { promisify } = require('util');
const networkParser = require('./networkParser');

const execAsync = promisify(exec);

/**
 * Collects system metrics (CPU, memory, load, network, ping)
 */
class MetricsService {
  constructor() {
    // Store previous network stats for rate calculation
    this.previousNetworkStats = null;
    this.previousNetworkTime = null;
  }

  /**
   * Get network bandwidth statistics (upload/download rates)
   * Reads from host /proc/net/dev to get actual host network usage
   * Aggregates only physical network interfaces (eth*, eno*, wlan*)
   */
  async getNetworkStats() {
    try {
      // Get total stats from host network interfaces
      const totalStats = await networkParser.getTotalStats();
      const currentTime = Date.now();

      const totalRxBytes = totalStats.rx_bytes;
      const totalTxBytes = totalStats.tx_bytes;

      // If we have previous stats, calculate rates
      if (this.previousNetworkStats && this.previousNetworkTime) {
        const timeDeltaSeconds = (currentTime - this.previousNetworkTime) / 1000;
        
        // Calculate bytes transferred since last measurement
        const rxDelta = totalRxBytes - this.previousNetworkStats.rx_bytes;
        const txDelta = totalTxBytes - this.previousNetworkStats.tx_bytes;
        
        // Calculate rates in Mbps (Megabits per second)
        // Formula: (bytes * 8 bits/byte) / (1024 * 1024) / seconds = Mbps
        const downloadMbps = (rxDelta * 8 / timeDeltaSeconds) / (1024 * 1024);
        const uploadMbps = (txDelta * 8 / timeDeltaSeconds) / (1024 * 1024);

        // Store current stats for next calculation
        this.previousNetworkStats = {
          rx_bytes: totalRxBytes,
          tx_bytes: totalTxBytes
        };
        this.previousNetworkTime = currentTime;

        return {
          uploadMbps: Math.max(0, parseFloat(uploadMbps.toFixed(2))),
          downloadMbps: Math.max(0, parseFloat(downloadMbps.toFixed(2))),
          interfaces: totalStats.interfaces // Include list of monitored interfaces
        };
      } else {
        // First measurement - store and return zeros
        this.previousNetworkStats = {
          rx_bytes: totalRxBytes,
          tx_bytes: totalTxBytes
        };
        this.previousNetworkTime = currentTime;
        return {
          uploadMbps: 0,
          downloadMbps: 0,
          interfaces: totalStats.interfaces
        };
      }
    } catch (error) {
      console.error('[metrics] Error collecting network stats:', error.message);
      return { uploadMbps: 0, downloadMbps: 0, interfaces: [] };
    }
  }

  /**
   * Ping kick.com and return latency
   */
  async pingKickCom() {
    try {
      // Execute ping command (1 count, 2 second timeout)
      const { stdout } = await execAsync('ping -c 1 -W 2 kick.com');
      
      // Parse the output to extract latency
      // Example line: "64 bytes from ... time=45.2 ms"
      const timeMatch = stdout.match(/time[=<](\d+\.?\d*)\s*ms/i);
      
      if (timeMatch && timeMatch[1]) {
        const latency = parseFloat(timeMatch[1]);
        return {
          host: 'kick.com',
          latency: parseFloat(latency.toFixed(1)),
          status: 'ok'
        };
      }
      
      // If we got output but couldn't parse time, return error
      return {
        host: 'kick.com',
        latency: 0,
        status: 'error'
      };
    } catch (error) {
      // Ping failed (network error, DNS failure, timeout, etc.)
      console.error('[metrics] Ping to kick.com failed:', error.message);
      return {
        host: 'kick.com',
        latency: 0,
        status: 'error'
      };
    }
  }

  async getSystemMetrics() {
    try {
      const [cpu, mem, load, network, ping] = await Promise.all([
        si.currentLoad(),
        si.mem(),
        si.currentLoad(),
        this.getNetworkStats(),
        this.pingKickCom()
      ]);

      return {
        cpu: parseFloat(cpu.currentLoad.toFixed(2)),
        memory: parseFloat(((mem.active / mem.total) * 100).toFixed(2)),
        load: cpu.avgLoad ? [
          parseFloat(cpu.avgLoad.toFixed(2))
        ] : [0],
        memoryUsed: Math.round(mem.active / (1024 * 1024 * 1024) * 100) / 100, // GB
        memoryTotal: Math.round(mem.total / (1024 * 1024 * 1024) * 100) / 100, // GB
        network,
        ping
      };
    } catch (error) {
      console.error('[metrics] Error collecting system metrics:', error.message);
      return {
        cpu: 0,
        memory: 0,
        load: [0],
        memoryUsed: 0,
        memoryTotal: 0,
        network: { uploadMbps: 0, downloadMbps: 0 },
        ping: { host: 'kick.com', latency: 0, status: 'error' }
      };
    }
  }
}

module.exports = new MetricsService();