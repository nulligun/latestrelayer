<template>
  <div class="stream-card">
    <h2>Streaming Statistics</h2>
    <div class="stream-info">
      <div class="info-item">
        <div class="info-label">Stream Status</div>
        <div class="info-value status-badge" :class="getStatusClass(streamStatus.isOnline)">
          {{ streamStatus.isOnline ? 'ONLINE' : 'OFFLINE' }}
        </div>
        <div class="duration-text">
          {{ formatDuration(streamStatus.durationSeconds) }}
        </div>
      </div>
      
      <div class="info-item">
        <div class="info-label">Current Scene</div>
        <div class="info-value scene-value">
          {{ currentScene ? currentScene.toUpperCase() : 'UNKNOWN' }}
        </div>
        <div class="duration-text">
          {{ formatDuration(sceneDurationSeconds) }}
        </div>
      </div>
    </div>
    
    <div class="streams-grid">
      <div
        v-for="(stream, name) in stats.streams"
        :key="name"
        class="stream-item"
        :class="{
          'current-scene': name === currentScene,
          'using-fallback': sourceAvailability && sourceAvailability[name] && sourceAvailability[name].using_fallback
        }"
      >
        <div class="stream-header">
          <span class="stream-name">{{ name }}</span>
          <div class="stream-header-right">
            <span v-if="sourceAvailability && sourceAvailability[name] && sourceAvailability[name].using_fallback"
                  class="fallback-badge"
                  title="Using fallback test source - waiting for real stream">
              ⚠ FALLBACK
            </span>
            <span v-if="switchingScene === name" class="switching-spinner"></span>
            <span class="stream-status" :class="{ active: stream.active, inactive: !stream.active }">
              {{ stream.active ? '● LIVE' : '○ OFFLINE' }}
            </span>
          </div>
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
        <button
          v-if="shouldShowSwitchButton(name)"
          @click="switchScene(name)"
          class="switch-button"
          :disabled="switching || switchingScene"
        >
          {{ switching === name ? 'Switching...' : `Switch to ${name}` }}
        </button>
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
    streamStatus: {
      type: Object,
      default: () => ({
        isOnline: false,
        durationSeconds: 0
      })
    },
    currentScene: {
      type: String,
      default: null
    },
    sceneDurationSeconds: {
      type: Number,
      default: 0
    },
    switchingScene: {
      type: String,
      default: null
    },
    sourceAvailability: {
      type: Object,
      default: null
    }
  },
  data() {
    return {
      switching: null
    };
  },
  methods: {
    getStatusClass(isOnline) {
      return isOnline ? 'status-online' : 'status-offline';
    },
    formatDuration(seconds) {
      if (seconds < 60) {
        return `${seconds}s`;
      } else if (seconds < 3600) {
        const minutes = Math.floor(seconds / 60);
        const secs = seconds % 60;
        return `${minutes}m ${secs}s`;
      } else {
        const hours = Math.floor(seconds / 3600);
        const minutes = Math.floor((seconds % 3600) / 60);
        const secs = seconds % 60;
        return `${hours}h ${minutes}m ${secs}s`;
      }
    },
    shouldShowSwitchButton(streamName) {
      // Don't show button for "program" stream or current scene
      return streamName !== 'program' && streamName !== this.currentScene;
    },
    async switchScene(sceneName) {
      console.log(`[StreamStats] switchScene called for: ${sceneName}`);
      console.log(`[StreamStats] switching=${this.switching}, switchingScene prop=${this.switchingScene}`);
      
      if (this.switching || this.switchingScene) {
        console.log(`[StreamStats] Blocked - already switching`);
        return;
      }
      
      this.switching = sceneName;
      console.log(`[StreamStats] Set local switching=${sceneName}`);
      
      // Emit event to parent to show loading indicator
      console.log(`[StreamStats] Emitting scene-switching event`);
      this.$emit('scene-switching', sceneName);
      
      try {
        // Use proxied endpoint to avoid CORS issues
        const response = await fetch('/api/scene/switch', {
          method: 'POST',
          headers: {
            'Content-Type': 'application/json'
          },
          body: JSON.stringify({ scene: sceneName })
        });
        
        if (!response.ok) {
          const errorData = await response.json();
          throw new Error(errorData.error || 'Switch failed');
        }
        
        const result = await response.json();
        console.log(`[StreamStats] Switch successful:`, result);
      } catch (error) {
        console.error(`[StreamStats] Error switching scene:`, error);
        alert(`Failed to switch scene: ${error.message}`);
      } finally {
        this.switching = null;
      }
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

.status-badge {
  display: inline-block;
  padding: 8px 16px;
  border-radius: 6px;
  font-size: 1rem;
}

.status-online {
  background: #10b981;
  color: #fff;
}

.status-offline {
  background: #ef4444;
  color: #fff;
}

.duration-text {
  margin-top: 8px;
  font-size: 0.875rem;
  color: #94a3b8;
  font-weight: normal;
}

.bandwidth-value {
  color: #3b82f6;
}

.scene-value {
  color: #10b981;
  font-weight: bold;
}

.streams-grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(250px, 1fr));
  gap: 15px;
}
.stream-item.using-fallback {
  border: 2px solid #f59e0b;
  box-shadow: 0 0 10px rgba(245, 158, 11, 0.3);
}

.fallback-badge {
  font-size: 0.65rem;
  font-weight: bold;
  padding: 3px 6px;
  border-radius: 3px;
  color: #fff;
  background: #f59e0b;
  margin-right: 6px;
}


.stream-item {
  background: #0f172a;
  border-radius: 6px;
  padding: 15px;
  border: 1px solid #334155;
  position: relative;
}

.stream-item.current-scene {
  border: 2px solid #10b981;
  box-shadow: 0 0 10px rgba(16, 185, 129, 0.3);
}

.stream-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 12px;
  padding-bottom: 12px;
  border-bottom: 1px solid #334155;
}

.stream-header-right {
  display: flex;
  align-items: center;
  gap: 8px;
}

.switching-spinner {
  width: 12px;
  height: 12px;
  border: 2px solid #3b82f6;
  border-top-color: transparent;
  border-radius: 50%;
  animation: spin 0.8s linear infinite;
}

@keyframes spin {
  to {
    transform: rotate(360deg);
  }
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

.switch-button {
  margin-top: 12px;
  width: 100%;
  padding: 8px 16px;
  background: #3b82f6;
  color: white;
  border: none;
  border-radius: 4px;
  font-size: 0.875rem;
  font-weight: 500;
  cursor: pointer;
  transition: background 0.2s;
}

.switch-button:hover:not(:disabled) {
  background: #2563eb;
}

.switch-button:active:not(:disabled) {
  background: #1d4ed8;
}

.switch-button:disabled {
  background: #64748b;
  cursor: not-allowed;
  opacity: 0.6;
}
</style>