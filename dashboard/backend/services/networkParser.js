const fs = require('fs').promises;
const path = require('path');

/**
 * Network Parser for reading host network interface statistics
 * Reads from mounted host /proc/net/dev to get actual host network usage
 */
class NetworkParser {
  constructor() {
    // Path to the mounted host /proc/net/dev file
    this.procNetDevPath = '/host_proc/net/dev';
    
    // Regex patterns for interface filtering
    this.physicalInterfacePattern = /^(eth|eno|enp|wlan|wlp)\d+/;
    this.excludeInterfacePattern = /^(lo|docker|br-|veth|virbr)/;
  }

  /**
   * Check if an interface should be included in statistics
   * @param {string} interfaceName - The network interface name
   * @returns {boolean} - True if interface should be included
   */
  shouldIncludeInterface(interfaceName) {
    // Exclude loopback and virtual interfaces
    if (this.excludeInterfacePattern.test(interfaceName)) {
      return false;
    }
    
    // Include only physical interfaces
    if (this.physicalInterfacePattern.test(interfaceName)) {
      return true;
    }
    
    return false;
  }

  /**
   * Parse a single line from /proc/net/dev
   * Format: interface: rx_bytes rx_packets ... tx_bytes tx_packets ...
   * @param {string} line - A line from /proc/net/dev
   * @returns {object|null} - Parsed interface stats or null if invalid
   */
  parseLine(line) {
    // Remove leading/trailing whitespace and split by whitespace
    const parts = line.trim().split(/\s+/);
    
    if (parts.length < 10) {
      return null;
    }
    
    // First part is "interface:" - extract interface name
    const interfaceWithColon = parts[0];
    const interfaceName = interfaceWithColon.replace(':', '').trim();
    
    if (!interfaceName) {
      return null;
    }
    
    // Check if we should include this interface
    if (!this.shouldIncludeInterface(interfaceName)) {
      return null;
    }
    
    // Parse statistics
    // Format after interface name:
    // [1]rx_bytes [2]rx_packets [3]rx_errs [4]rx_drop [5]rx_fifo [6]rx_frame [7]rx_compressed [8]rx_multicast
    // [9]tx_bytes [10]tx_packets [11]tx_errs [12]tx_drop [13]tx_fifo [14]tx_colls [15]tx_carrier [16]tx_compressed
    const rxBytes = parseInt(parts[1], 10);
    const txBytes = parseInt(parts[9], 10);
    
    if (isNaN(rxBytes) || isNaN(txBytes)) {
      return null;
    }

    console.log(`[networkParser] Parsed interface ${interfaceName}: rx_bytes=${rxBytes}, tx_bytes=${txBytes}`);
    
    return {
      interface: interfaceName,
      rx_bytes: rxBytes,
      tx_bytes: txBytes
    };
  }

  /**
   * Read and parse the host network interface statistics
   * @returns {Promise<Array>} - Array of interface statistics
   */
  async parseNetworkInterfaces() {
    try {
      // Read the /proc/net/dev file
      const data = await fs.readFile(this.procNetDevPath, 'utf8');
      
      // Split into lines
      const lines = data.split('\n');
      
      // Skip first two header lines
      const dataLines = lines.slice(2);
      
      // Parse each line and filter out nulls
      const interfaces = [];
      for (const line of dataLines) {
        if (line.trim() === '') {
          continue;
        }
        
        const parsed = this.parseLine(line);
        if (parsed) {
          interfaces.push(parsed);
        }
      }
      
      return interfaces;
    } catch (error) {
      // If file doesn't exist or can't be read, log error and return empty array
      if (error.code === 'ENOENT') {
        console.error('[networkParser] Host /proc/net/dev not mounted. Please ensure volume is mounted.');
      } else {
        console.error('[networkParser] Error reading host network stats:', error.message);
      }
      return [];
    }
  }

  /**
   * Get total network statistics across all physical interfaces
   * @returns {Promise<object>} - Total rx_bytes and tx_bytes
   */
  async getTotalStats() {
    const interfaces = await this.parseNetworkInterfaces();
    
    let totalRxBytes = 0;
    let totalTxBytes = 0;
    
    for (const iface of interfaces) {
      totalRxBytes += iface.rx_bytes;
      totalTxBytes += iface.tx_bytes;
    }
    
    return {
      rx_bytes: totalRxBytes,
      tx_bytes: totalTxBytes,
      interfaces: interfaces.map(i => i.interface) // Return list of included interfaces
    };
  }
}

module.exports = new NetworkParser();