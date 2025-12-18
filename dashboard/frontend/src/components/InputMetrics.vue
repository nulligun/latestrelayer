<template>
  <div class="metrics-card">
    <h2>Input Metrics</h2>
    <div class="metrics-grid">
      <!-- Fallback Input -->
      <div class="metric">
        <div class="metric-label">Fallback</div>
        <div class="metric-status" :class="getStatusClass(metrics.fallback)">
          {{ getStatusText(metrics.fallback) }}
        </div>
        <div class="metric-value" :class="getBitrateClass(metrics.fallback.bitrate_kbps)">
          {{ metrics.fallback.bitrate_kbps.toFixed(0) }} Kbps
        </div>
        <div class="metric-bar">
          <div class="metric-bar-fill" :style="{ width: `${Math.min((metrics.fallback.bitrate_kbps / 50), 100)}%`, backgroundColor: getBitrateColor(metrics.fallback.bitrate_kbps) }"></div>
        </div>
        <div class="metric-detail">
          Data age: {{ formatDataAge(metrics.fallback.data_age_ms) }}
        </div>
      </div>

      <!-- Camera Input -->
      <div class="metric">
        <div class="metric-label">Camera</div>
        <div class="metric-status" :class="getStatusClass(metrics.camera)">
          {{ getStatusText(metrics.camera) }}
        </div>
        <div class="metric-value" :class="getBitrateClass(metrics.camera.bitrate_kbps)">
          {{ metrics.camera.bitrate_kbps.toFixed(0) }} Kbps
        </div>
        <div class="metric-bar">
          <div class="metric-bar-fill" :style="{ width: `${Math.min((metrics.camera.bitrate_kbps / 50), 100)}%`, backgroundColor: getBitrateColor(metrics.camera.bitrate_kbps) }"></div>
        </div>
        <div class="metric-detail">
          Data age: {{ formatDataAge(metrics.camera.data_age_ms) }}
        </div>
      </div>

      <!-- Drone Input -->
      <div class="metric">
        <div class="metric-label">Drone</div>
        <div class="metric-status" :class="getStatusClass(metrics.drone)">
          {{ getStatusText(metrics.drone) }}
        </div>
        <div class="metric-value" :class="getBitrateClass(metrics.drone.bitrate_kbps)">
          {{ metrics.drone.bitrate_kbps.toFixed(0) }} Kbps
        </div>
        <div class="metric-bar">
          <div class="metric-bar-fill" :style="{ width: `${Math.min((metrics.drone.bitrate_kbps / 50), 100)}%`, backgroundColor: getBitrateColor(metrics.drone.bitrate_kbps) }"></div>
        </div>
        <div class="metric-detail">
          Data age: {{ formatDataAge(metrics.drone.data_age_ms) }}
        </div>
      </div>
    </div>
  </div>
</template>

<script>
export default {
  name: 'InputMetrics',
  props: {
    metrics: {
      type: Object,
      default: () => ({
        fallback: { connected: false, bitrate_kbps: 0, data_age_ms: -1 },
        camera: { connected: false, bitrate_kbps: 0, data_age_ms: -1 },
        drone: { connected: false, bitrate_kbps: 0, data_age_ms: -1 }
      })
    }
  },
  methods: {
    getStatusClass(input) {
      if (!input.connected) return 'status-disconnected';
      if (input.data_age_ms < 0) return 'status-no-data';
      if (input.data_age_ms > 3000) return 'status-stale';
      return 'status-connected';
    },
    getStatusText(input) {
      if (!input.connected) return 'Disconnected';
      if (input.data_age_ms < 0) return 'No Data';
      if (input.data_age_ms > 3000) return 'Stale';
      return 'Connected';
    },
    getBitrateClass(bitrate) {
      if (bitrate === 0) return 'metric-danger';
      if (bitrate < 1000) return 'metric-warning';
      return 'metric-good';
    },
    getBitrateColor(bitrate) {
      if (bitrate === 0) return '#ef4444';
      if (bitrate < 1000) return '#f59e0b';
      return '#10b981';
    },
    formatDataAge(ageMs) {
      if (ageMs < 0) return 'N/A';
      if (ageMs < 1000) return `${ageMs}ms`;
      return `${(ageMs / 1000).toFixed(1)}s`;
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
  grid-template-columns: repeat(3, 1fr);
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
  font-weight: 600;
}

.metric-status {
  font-size: 0.75rem;
  font-weight: 600;
  margin-bottom: 8px;
  padding: 2px 8px;
  border-radius: 4px;
  display: inline-block;
}

.status-connected {
  background: rgba(16, 185, 129, 0.2);
  color: #10b981;
}

.status-disconnected {
  background: rgba(239, 68, 68, 0.2);
  color: #ef4444;
}

.status-no-data {
  background: rgba(100, 116, 139, 0.2);
  color: #64748b;
}

.status-stale {
  background: rgba(245, 158, 11, 0.2);
  color: #f59e0b;
}

.metric-value {
  font-size: 1.5rem;
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

@media (max-width: 1024px) {
  .metrics-grid {
    grid-template-columns: 1fr;
  }
}
</style>