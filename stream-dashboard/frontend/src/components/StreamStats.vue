<template>
  <div class="stream-card">
    <h2>Streaming Statistics</h2>
    <div class="stream-info">
      <div class="info-item">
        <div class="info-label">Current Scene</div>
        <div class="info-value scene-badge" :class="getSceneClass(currentScene)">
          {{ getSceneDisplay(currentScene) }}
        </div>
      </div>
      
      <div class="info-item">
        <div class="info-label">Total Inbound Bandwidth</div>
        <div class="info-value bandwidth-value">
          {{ stats.inboundBandwidth.toLocaleString() }} kbps
        </div>
      </div>
    </div>
    
    <div class="streams-grid">
      <div v-for="(stream, name) in stats.streams" :key="name" class="stream-item">
        <div class="stream-header">
          <span class="stream-name">{{ name }}</span>
          <span class="stream-status" :class="{ active: stream.active, inactive: !stream.active }">
            {{ stream.active ? '● LIVE' : '○ OFFLINE' }}
          </span>
        </div>
        <div class="stream-details">
          <div class="detail">
            <span class="detail-label">Bandwidth:</span>
            <span class="detail-value">{{ stream.bandwidth.toLocaleString() }} kbps</span>
          </div>
          <div class="detail">
            <span class="detail-label">Clients:</span>
            <span class="detail-value">{{ stream.clients }}</span>
          </div>
          <div class="detail">
            <span class="detail-label">Publishing:</span>
            <span class="detail-value">{{ stream.publishing ? 'Yes' : 'No' }}</span>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<script>
export default {
  name: 'StreamStats',
  props: {
    stats: {
      type: Object,
      default: () => ({
        inboundBandwidth: 0,
        streams: {}
      })
    },
    currentScene: {
      type: String,
      default: null
    }
  },
  methods: {
    getSceneDisplay(scene) {
      if (!scene) return 'Unknown';
      return scene.toUpperCase();
    },
    getSceneClass(scene) {
      if (scene === 'cam') return 'scene-cam';
      if (scene === 'offline') return 'scene-offline';
      return 'scene-unknown';
    }
  }
};
</script>

<style scoped>
.stream-card {
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

.stream-info {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(250px, 1fr));
  gap: 20px;
  margin-bottom: 20px;
}

.info-item {
  background: #0f172a;
  border-radius: 6px;
  padding: 15px;
}

.info-label {
  font-size: 0.875rem;
  color: #94a3b8;
  margin-bottom: 8px;
}

.info-value {
  font-size: 1.5rem;
  font-weight: bold;
  color: #f1f5f9;
}

.scene-badge {
  display: inline-block;
  padding: 8px 16px;
  border-radius: 6px;
  font-size: 1rem;
}

.scene-cam {
  background: #10b981;
  color: #fff;
}

.scene-offline {
  background: #f59e0b;
  color: #fff;
}

.scene-unknown {
  background: #64748b;
  color: #fff;
}

.bandwidth-value {
  color: #3b82f6;
}

.streams-grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(250px, 1fr));
  gap: 15px;
}

.stream-item {
  background: #0f172a;
  border-radius: 6px;
  padding: 15px;
  border: 1px solid #334155;
}

.stream-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 12px;
  padding-bottom: 12px;
  border-bottom: 1px solid #334155;
}

.stream-name {
  font-weight: bold;
  color: #f1f5f9;
  text-transform: uppercase;
  font-size: 0.875rem;
}

.stream-status {
  font-size: 0.75rem;
  font-weight: bold;
  padding: 4px 8px;
  border-radius: 4px;
}

.stream-status.active {
  color: #10b981;
  background: rgba(16, 185, 129, 0.1);
}

.stream-status.inactive {
  color: #64748b;
  background: rgba(100, 116, 139, 0.1);
}

.stream-details {
  display: flex;
  flex-direction: column;
  gap: 8px;
}

.detail {
  display: flex;
  justify-content: space-between;
  font-size: 0.875rem;
}

.detail-label {
  color: #94a3b8;
}

.detail-value {
  color: #e2e8f0;
  font-weight: 500;
}
</style>