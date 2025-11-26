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
    }
  },
  methods: {
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
</style>