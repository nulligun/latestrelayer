<template>
  <div class="stream-card">
    <h2>Camera Details</h2>
    
    <!-- Camera Details - 3 Column Grid -->
    <div class="camera-details-grid">
      <!-- Column 1: Copy Button + RTMP URL -->
      <div class="info-item url-item">
        <div class="info-label">CAMERA URL</div>
        <div class="url-with-copy">
          <button
            class="copy-button"
            :class="{ copied: copySuccess }"
            @click="copyToClipboard"
            :disabled="!srtUrl"
          >
            {{ copySuccess ? 'Copied!' : 'Copy' }}
          </button>
          <div class="url-display">{{ srtUrl }}</div>
        </div>
      </div>
      
      <!-- Column 2: Stream Status -->
      <div class="info-item">
        <div class="info-label">Stream Status</div>
        <div class="info-value status-badge" :class="isKickLive ? 'status-kick-live' : 'status-kick-offline'">
          {{ isKickLive ? '🔴 LIVE ON KICK' : 'KICK OFFLINE' }}
        </div>
        <div v-if="isKickLive" class="duration-text">
          {{ formatDuration(streamStatus.durationSeconds) }}
        </div>
        <div v-if="streamStatus.srtBitrateKbps > 0" class="duration-text">
          Bitrate: {{ streamStatus.srtBitrateKbps }} Kbps
        </div>
        <div v-else-if="switcherHealth?.srt_connected" class="duration-text">
          Bitrate: Calculating...
        </div>
      </div>
      
      <!-- Column 3: Current Scene -->
      <div class="info-item">
        <div class="info-label">Current Scene</div>
        <div class="info-value scene-value">
          {{ displayScene }}
        </div>
        <div class="duration-text">
          {{ formatDuration(sceneDurationSeconds) }}
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
    cameraConfig: {
      type: Object,
      default: null
    },
    switcherHealth: {
      type: Object,
      default: () => ({})
    },
    fallbackConfig: {
      type: Object,
      default: () => ({
        source: 'BLACK',
        imagePath: '/app/shared/offline.png',
        videoPath: '/app/shared/offline.mp4',
        browserUrl: 'https://example.com'
      })
    }
  },
  data() {
    return {
      localSrtUrl: '',
      copySuccess: false
    };
  },
  computed: {
    isKickLive() {
      return this.switcherHealth?.kick_streaming_enabled || false;
    },
    srtUrl() {
      return this.cameraConfig?.srtUrl || this.localSrtUrl || 'Loading...';
    },
    displayScene() {
      if (!this.currentScene) return 'Unknown';
      
      const scene = this.currentScene.toUpperCase();
      const privacyEnabled = this.switcherHealth?.privacy_enabled || false;
      
      // New scene values from multiplexer: LIVE, FALLBACK, unknown
      if (scene === 'LIVE') {
        return 'Camera';
      } else if (scene === 'FALLBACK') {
        if (privacyEnabled) {
          return 'Privacy Mode';
        }
        // Show fallback source based on configuration
        const source = this.fallbackConfig?.source || 'BLACK';
        if (source === 'IMAGE') {
          return 'Fallback: Static Image';
        } else if (source === 'VIDEO') {
          return 'Fallback: Video Loop';
        } else if (source === 'BROWSER') {
          return 'Fallback: Web Browser';
        }
        return 'Fallback';
      } else if (scene === 'UNKNOWN') {
        return 'Camera Not Connected';
      }
      
      // Legacy support for old scene names (SRT/VIDEO/BLACK)
      if (scene === 'SRT') {
        return 'Camera';
      } else if (scene === 'BLACK') {
        return privacyEnabled ? 'Privacy Mode' : 'Black Screen';
      } else if (scene === 'VIDEO') {
        if (privacyEnabled) {
          return 'Privacy Mode';
        }
        const source = this.fallbackConfig?.source || 'BLACK';
        if (source === 'IMAGE') {
          return 'Fallback: Static Image';
        } else if (source === 'VIDEO') {
          return 'Fallback: Video Loop';
        } else if (source === 'BROWSER') {
          return 'Fallback: Web Browser';
        }
        return 'Fallback: Black Screen';
      }
      
      // Return original scene name for any unknown scenes
      return scene;
    }
  },
  mounted() {
    if (!this.cameraConfig) {
      this.fetchSrtConfig();
    }
  },
  methods: {
    async fetchSrtConfig() {
      try {
        const response = await fetch('/api/config');
        const config = await response.json();
        this.localSrtUrl = config.srtUrl;
      } catch (error) {
        console.error('[StreamStats] Error fetching SRT config:', error);
        this.localSrtUrl = 'Error loading URL';
      }
    },
    async copyToClipboard() {
      if (!this.srtUrl || this.srtUrl === 'Error loading URL') return;
      
      try {
        await navigator.clipboard.writeText(this.srtUrl);
        this.copySuccess = true;
        setTimeout(() => {
          this.copySuccess = false;
        }, 2000);
      } catch (error) {
        console.error('Failed to copy to clipboard:', error);
        const textArea = document.createElement('textarea');
        textArea.value = this.srtUrl;
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

.camera-details-grid {
  display: grid;
  grid-template-columns: repeat(3, 1fr);
  gap: 20px;
}

@media (max-width: 1200px) {
  .camera-details-grid {
    grid-template-columns: 1fr;
  }
}

.info-item {
  background: #0f172a;
  border-radius: 6px;
  padding: 15px;
}

.url-item {
  display: flex;
  flex-direction: column;
  gap: 10px;
}

.info-label {
  font-size: 0.875rem;
  color: #94a3b8;
  margin-bottom: 8px;
}

.url-with-copy {
  display: flex;
  gap: 12px;
  align-items: center;
}

.copy-button {
  padding: 12px 20px;
  background: #3b82f6;
  color: white;
  border: none;
  border-radius: 6px;
  font-size: 0.875rem;
  font-weight: 600;
  cursor: pointer;
  transition: all 0.2s ease;
  white-space: nowrap;
  min-width: 80px;
  flex-shrink: 0;
}

.copy-button:hover:not(:disabled) {
  background: #2563eb;
  transform: translateY(-1px);
  box-shadow: 0 4px 8px rgba(59, 130, 246, 0.3);
}

.copy-button:active:not(:disabled) {
  background: #1d4ed8;
  transform: translateY(0);
  box-shadow: 0 2px 4px rgba(59, 130, 246, 0.3);
}

.copy-button.copied {
  background: #10b981;
  color: white;
}

.copy-button.copied:hover {
  background: #059669;
}

.copy-button:disabled {
  background: #475569;
  cursor: not-allowed;
  opacity: 0.6;
}

.url-display {
  flex: 1;
  background: #0f172a;
  border: 1px solid #334155;
  border-radius: 6px;
  padding: 12px 16px;
  font-family: 'Courier New', monospace;
  font-size: 0.875rem;
  color: #e2e8f0;
  word-break: break-all;
  display: flex;
  align-items: center;
  min-width: 0;
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

.status-kick-live {
  background: #ef4444;
  color: #fff;
  animation: pulse-kick 2s ease-in-out infinite;
}

.status-kick-offline {
  background: #64748b;
  color: #e2e8f0;
}

@keyframes pulse-kick {
  0%, 100% {
    box-shadow: 0 0 0 0 rgba(239, 68, 68, 0.7);
  }
  50% {
    box-shadow: 0 0 0 8px rgba(239, 68, 68, 0);
  }
}

.duration-text {
  margin-top: 8px;
  font-size: 0.875rem;
  color: #94a3b8;
  font-weight: normal;
}

.scene-value {
  color: #10b981;
  font-weight: bold;
}
</style>