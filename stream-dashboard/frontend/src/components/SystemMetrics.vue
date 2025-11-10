<template>
  <div class="metrics-card">
    <h2>System Metrics</h2>
    <div class="metrics-grid">
      <div class="metric">
        <div class="metric-label">CPU Usage</div>
        <div class="metric-value" :class="getCpuClass(metrics.cpu)">
          {{ metrics.cpu.toFixed(1) }}%
        </div>
        <div class="metric-bar">
          <div class="metric-bar-fill" :style="{ width: `${metrics.cpu}%`, backgroundColor: getCpuColor(metrics.cpu) }"></div>
        </div>
      </div>
      
      <div class="metric">
        <div class="metric-label">Memory Usage</div>
        <div class="metric-value" :class="getMemoryClass(metrics.memory)">
          {{ metrics.memory.toFixed(1) }}%
        </div>
        <div class="metric-bar">
          <div class="metric-bar-fill" :style="{ width: `${metrics.memory}%`, backgroundColor: getMemoryColor(metrics.memory) }"></div>
        </div>
        <div class="metric-detail">
          {{ metrics.memoryUsed.toFixed(2) }} GB / {{ metrics.memoryTotal.toFixed(2) }} GB
        </div>
      </div>
      
      <div class="metric">
        <div class="metric-label">System Load</div>
        <div class="metric-value">
          {{ metrics.load[0]?.toFixed(2) || '0.00' }}
        </div>
      </div>

      <div class="metric">
        <div class="metric-label">Network Upload</div>
        <div class="metric-value" :class="getNetworkClass(metrics.network?.uploadMbps || 0)">
          {{ (metrics.network?.uploadMbps || 0).toFixed(2) }} Mbps
        </div>
        <div class="metric-bar">
          <div class="metric-bar-fill" :style="{ width: `${Math.min((metrics.network?.uploadMbps || 0) * 5, 100)}%`, backgroundColor: getNetworkColor(metrics.network?.uploadMbps || 0) }"></div>
        </div>
      </div>

      <div class="metric">
        <div class="metric-label">Network Download</div>
        <div class="metric-value" :class="getNetworkClass(metrics.network?.downloadMbps || 0)">
          {{ (metrics.network?.downloadMbps || 0).toFixed(2) }} Mbps
        </div>
        <div class="metric-bar">
          <div class="metric-bar-fill" :style="{ width: `${Math.min((metrics.network?.downloadMbps || 0) * 5, 100)}%`, backgroundColor: getNetworkColor(metrics.network?.downloadMbps || 0) }"></div>
        </div>
      </div>

      <div class="metric">
        <div class="metric-label">Ping to kick.com</div>
        <div v-if="metrics.ping?.status === 'ok'" class="metric-value" :class="getPingClass(metrics.ping?.latency || 0)">
          {{ (metrics.ping?.latency || 0).toFixed(1) }} ms
        </div>
        <div v-else class="metric-value metric-danger">
          Error
        </div>
        <div v-if="metrics.ping?.status === 'ok'" class="metric-bar">
          <div class="metric-bar-fill" :style="{ width: `${Math.min((metrics.ping?.latency || 0) / 2, 100)}%`, backgroundColor: getPingColor(metrics.ping?.latency || 0) }"></div>
        </div>
      </div>
    </div>

    <div class="rtmp-url-section">
      <h3 class="section-title">Camera Address</h3>
      <div class="url-container">
        <div class="url-display">{{ rtmpUrl }}</div>
        <button
          class="copy-button"
          :class="{ copied: copySuccess }"
          @click="copyToClipboard"
          :disabled="!rtmpUrl"
        >
          {{ copySuccess ? 'Copied!' : 'Copy' }}
        </button>
      </div>
      
      <div v-if="statusSuccess.length > 0" class="status-success">
        <div
          v-for="(success, index) in statusSuccess"
          :key="index"
          class="success-item"
        >
          <span class="success-icon">✅</span>
          {{ success }}
        </div>
      </div>
      
      <div v-if="statusWarnings.length > 0" class="status-warnings">
        <div
          v-for="(warning, index) in statusWarnings"
          :key="index"
          class="warning-item"
        >
          <span class="warning-icon">⛔</span>
          {{ warning }}
        </div>
      </div>
    </div>
  </div>
</template>

<script>
export default {
  name: 'SystemMetrics',
  props: {
    metrics: {
      type: Object,
      default: () => ({
        cpu: 0,
        memory: 0,
        load: [0],
        memoryUsed: 0,
        memoryTotal: 0,
        network: {
          uploadMbps: 0,
          downloadMbps: 0
        },
        ping: {
          host: 'kick.com',
          latency: 0,
          status: 'error'
        }
      })
    },
    containers: {
      type: Array,
      default: () => []
    },
    rtmpStats: {
      type: Object,
      default: () => ({
        streams: {}
      })
    }
  },
  computed: {
    statusWarnings() {
      const warnings = [];
      
      // Check if ffmpeg-kick is running AND healthy
      const kickContainer = this.containers.find(c => c.name === 'ffmpeg-kick');
      if (!kickContainer || !kickContainer.running || kickContainer.health === 'unhealthy') {
        warnings.push('NOT LIVE ON KICK!');
      }
      
      // Check if auto-switcher is running
      const autoSwitcherContainer = this.containers.find(c => c.name === 'auto-switcher');
      if (!autoSwitcherContainer || !autoSwitcherContainer.running) {
        warnings.push('Auto Scene Switching NOT RUNNING');
      }
      
      // Check if camera stream is connected
      const camStream = this.rtmpStats?.streams?.cam;
      if (!camStream || !camStream.active || !camStream.publishing) {
        warnings.push('CAMERA NOT CONNECTED');
      }
      
      // Check if program stream exists
      const programStream = this.rtmpStats?.streams?.program;
      if (!programStream) {
        warnings.push('The program stream is missing. Please restart Muxer');
      } else if (!programStream.active || !programStream.publishing) {
        if (!kickContainer || !kickContainer.running) {
          warnings.push('Program stream not connected. Waiting to go live.');
        } else {
          warnings.push('Program stream NOT CONNECTED. Restart muxer.');
        }
      }
      
      return warnings;
    },
    statusSuccess() {
      const successes = [];
      
      // Check if ffmpeg-kick is running AND healthy
      const kickContainer = this.containers.find(c => c.name === 'ffmpeg-kick');
      if (kickContainer && kickContainer.running && kickContainer.health === 'healthy') {
        successes.push("WE'RE LIVE ON KICK!");
      }
      
      return successes;
    }
  },
  data() {
    return {
      rtmpUrl: '',
      copySuccess: false
    };
  },
  mounted() {
    this.fetchRtmpConfig();
  },
  methods: {
    async fetchRtmpConfig() {
      try {
        const response = await fetch('/api/config');
        const config = await response.json();
        this.rtmpUrl = config.rtmpUrl;
      } catch (error) {
        console.error('Error fetching RTMP config:', error);
        this.rtmpUrl = 'Error loading URL';
      }
    },
    async copyToClipboard() {
      if (!this.rtmpUrl || this.rtmpUrl === 'Error loading URL') return;
      
      try {
        await navigator.clipboard.writeText(this.rtmpUrl);
        this.copySuccess = true;
        setTimeout(() => {
          this.copySuccess = false;
        }, 2000);
      } catch (error) {
        console.error('Failed to copy to clipboard:', error);
        // Fallback for older browsers
        const textArea = document.createElement('textarea');
        textArea.value = this.rtmpUrl;
        textArea.style.position = 'fixed';
        textArea.style.left = '-999999px';
        document.body.appendChild(textArea);
        textArea.focus();
        textArea.select();
        try {
          document.execCommand('copy');
          this.copySuccess = true;
          setTimeout(() => {
            this.copySuccess = false;
          }, 2000);
        } catch (err) {
          console.error('Fallback copy failed:', err);
        }
        document.body.removeChild(textArea);
      }
    },
    getCpuClass(cpu) {
      if (cpu > 80) return 'metric-danger';
      if (cpu > 60) return 'metric-warning';
      return 'metric-good';
    },
    getCpuColor(cpu) {
      if (cpu > 80) return '#ef4444';
      if (cpu > 60) return '#f59e0b';
      return '#10b981';
    },
    getMemoryClass(memory) {
      if (memory > 85) return 'metric-danger';
      if (memory > 70) return 'metric-warning';
      return 'metric-good';
    },
    getMemoryColor(memory) {
      if (memory > 85) return '#ef4444';
      if (memory > 70) return '#f59e0b';
      return '#10b981';
    },
    getNetworkClass(mbps) {
      if (mbps > 20) return 'metric-danger';
      if (mbps > 5) return 'metric-warning';
      return 'metric-good';
    },
    getNetworkColor(mbps) {
      if (mbps > 20) return '#ef4444';
      if (mbps > 5) return '#f59e0b';
      return '#10b981';
    },
    getPingClass(latency) {
      if (latency > 200) return 'metric-danger';
      if (latency > 100) return 'metric-warning';
      if (latency > 50) return 'metric-warning';
      return 'metric-good';
    },
    getPingColor(latency) {
      if (latency > 200) return '#ef4444';
      if (latency > 100) return '#f59e0b';
      if (latency > 50) return '#f59e0b';
      return '#10b981';
    }
  }
};
</script>

<style scoped>
.metrics-card {
  background: #1e293b;
  border-radius: 8px;
  padding: 20px;
  box-shadow: 0 4px 6px rgba(0, 0, 0, 0.1);
}

h2 {
  margin: 0 0 20px 0;
  font-size: 1.25rem;
  color: #f1f5f9;
}

.metrics-grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
  gap: 20px;
}

.metric {
  background: #0f172a;
  border-radius: 6px;
  padding: 15px;
}

.metric-label {
  font-size: 0.875rem;
  color: #94a3b8;
  margin-bottom: 8px;
}

.metric-value {
  font-size: 2rem;
  font-weight: bold;
  margin-bottom: 10px;
}

.metric-good {
  color: #10b981;
}

.metric-warning {
  color: #f59e0b;
}

.metric-danger {
  color: #ef4444;
}

.metric-bar {
  height: 8px;
  background: #334155;
  border-radius: 4px;
  overflow: hidden;
  margin-bottom: 8px;
}

.metric-bar-fill {
  height: 100%;
  transition: width 0.3s ease;
}

.metric-detail {
  font-size: 0.75rem;
  color: #64748b;
}

.rtmp-url-section {
  margin-top: 20px;
  padding-top: 20px;
  border-top: 1px solid #334155;
}

.section-title {
  font-size: 1rem;
  color: #f1f5f9;
  margin-bottom: 12px;
  font-weight: 600;
}

.url-container {
  display: flex;
  gap: 10px;
  align-items: center;
  margin-top: 10px;
}

.url-display {
  flex: 1;
  background: #0f172a;
  border: 1px solid #334155;
  border-radius: 6px;
  padding: 12px 15px;
  font-family: 'Courier New', Courier, monospace;
  font-size: 0.875rem;
  color: #3b82f6;
  word-break: break-all;
  overflow-x: auto;
  white-space: nowrap;
}

.copy-button {
  flex-shrink: 0;
  padding: 12px 24px;
  background: #3b82f6;
  color: white;
  border: none;
  border-radius: 4px;
  font-size: 0.875rem;
  font-weight: 500;
  cursor: pointer;
  transition: all 0.2s;
  min-width: 85px;
}

.copy-button:hover:not(:disabled) {
  background: #2563eb;
}

.copy-button:active:not(:disabled) {
  transform: scale(0.98);
}

.copy-button:disabled {
  background: #475569;
  cursor: not-allowed;
  opacity: 0.5;
}

.copy-button.copied {
  background: #10b981;
}

.copy-button.copied:hover {
  background: #059669;
}

.status-success {
  margin-top: 15px;
  display: flex;
  flex-direction: column;
  gap: 10px;
}

.success-item {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 12px 15px;
  background: rgba(16, 185, 129, 0.1);
  border: 1px solid #10b981;
  border-radius: 6px;
  color: #10b981;
  font-size: 0.875rem;
  font-weight: 600;
}

.success-icon {
  font-size: 1.2rem;
  flex-shrink: 0;
}

.status-warnings {
  margin-top: 15px;
  display: flex;
  flex-direction: column;
  gap: 10px;
}

.warning-item {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 12px 15px;
  background: rgba(239, 68, 68, 0.1);
  border: 1px solid #ef4444;
  border-radius: 6px;
  color: #ef4444;
  font-size: 0.875rem;
  font-weight: 600;
}

.warning-icon {
  font-size: 1.2rem;
  flex-shrink: 0;
}
</style>