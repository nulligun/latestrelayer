<template>
  <div class="stream-card">
    <h2>Camera Details</h2>
    
    <!-- Camera Details - 3 Row Grid -->
    <div class="camera-details-grid">
      <!-- Row 0: Encoder Settings (full width) -->
      <div class="info-item encoder-settings-item">
        <div class="info-label">ENCODER SETTINGS</div>
        <div class="encoder-settings-value">{{ encoderSettingsDisplay }}</div>
      </div>
      
      <!-- Row 1: URLs -->
      <!-- Preview URL -->
      <div class="info-item url-item">
        <div class="info-label">PREVIEW URL</div>
        <div class="url-with-copy">
          <button
            class="copy-button"
            :class="{ copied: copyPreviewSuccess }"
            @click="copyPreviewToClipboard"
            :disabled="!previewUrl"
          >
            {{ copyPreviewSuccess ? 'Copied!' : 'Copy' }}
          </button>
          <div class="url-display">{{ displayPreviewUrl }}</div>
          <button
            class="toggle-visibility-btn"
            @click="togglePreviewUrlVisibility"
            type="button"
            :title="showPreviewUrl ? 'Hide URL' : 'Show URL'"
          >
            {{ showPreviewUrl ? '🔓' : '🔒' }}
          </button>
        </div>
      </div>
      
      <!-- Camera URL -->
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
          <div class="url-display">{{ displayCameraUrl }}</div>
          <button
            class="toggle-visibility-btn"
            @click="toggleCameraUrlVisibility"
            type="button"
            :title="showCameraUrl ? 'Hide URL' : 'Show URL'"
          >
            {{ showCameraUrl ? '🔓' : '🔒' }}
          </button>
        </div>
      </div>
      
      <!-- Drone URL -->
      <div class="info-item url-item">
        <div class="info-label">DRONE URL</div>
        <div class="url-with-copy">
          <button
            class="copy-button"
            :class="{ copied: copyDroneSuccess }"
            @click="copyDroneToClipboard"
            :disabled="!droneUrl"
          >
            {{ copyDroneSuccess ? 'Copied!' : 'Copy' }}
          </button>
          <div class="url-display">{{ displayDroneUrl }}</div>
          <button
            class="toggle-visibility-btn"
            @click="toggleDroneUrlVisibility"
            type="button"
            :title="showDroneUrl ? 'Hide URL' : 'Show URL'"
          >
            {{ showDroneUrl ? '🔓' : '🔒' }}
          </button>
        </div>
      </div>
      
      <!-- Row 2: Status, Scene, and Camera Source -->
      <!-- Stream Status -->
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
      
      <!-- Current Scene -->
      <div class="info-item">
        <div class="info-label">Current Scene</div>
        <div class="info-value scene-value">
          {{ displayScene }}
        </div>
        <div class="duration-text">
          {{ formatDuration(sceneDurationSeconds) }}
        </div>
      </div>
      
      <!-- Camera Source -->
      <div class="info-item">
        <div class="info-label">Camera Source</div>
        <div class="source-select-container">
          <select
            v-model="selectedSource"
            @change="onSourceChange"
            :disabled="sourceChangePending"
            class="source-select"
          >
            <option value="camera">CAMERA</option>
            <option value="drone">DRONE</option>
          </select>
        </div>
        <div v-if="sourceChangePending" class="duration-text">
          Switching...
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
    },
    currentSource: {
      type: String,
      default: 'camera'
    }
  },
  data() {
    return {
      localSrtUrl: '',
      localPreviewUrl: '',
      localDroneUrl: '',
      localEncodingSettings: null,
      copySuccess: false,
      copyPreviewSuccess: false,
      copyDroneSuccess: false,
      showPreviewUrl: false,
      showCameraUrl: false,
      showDroneUrl: false,
      selectedSource: 'camera',
      sourceChangePending: false
    };
  },
  computed: {
    isKickLive() {
      return this.switcherHealth?.kick_streaming_enabled || false;
    },
    srtUrl() {
      return this.cameraConfig?.srtUrl || this.localSrtUrl || 'Loading...';
    },
    previewUrl() {
      return this.cameraConfig?.previewUrl || this.localPreviewUrl || 'Loading...';
    },
    droneUrl() {
      return this.cameraConfig?.droneUrl || this.localDroneUrl || 'Loading...';
    },
    displayPreviewUrl() {
      if (this.showPreviewUrl) {
        return this.previewUrl;
      }
      // Return masked text when hidden
      return '••••••••••••••••••••••••••••••';
    },
    displayCameraUrl() {
      if (this.showCameraUrl) {
        return this.srtUrl;
      }
      // Return masked text when hidden
      return '••••••••••••••••••••••••••••••';
    },
    displayDroneUrl() {
      if (this.showDroneUrl) {
        return this.droneUrl;
      }
      // Return masked text when hidden
      return '••••••••••••••••••••••••••••••';
    },
    encoderSettingsDisplay() {
      const config = this.cameraConfig?.encodingSettings || this.localEncodingSettings;
      if (!config) return 'Loading...';
      
      // Transform libx264 → h264 for user-friendly display
      const videoCodec = config.videoEncoder === 'libx264' ? 'h264' : config.videoEncoder;
      
      // Format audio sample rate as kHz
      const audioSampleKHz = Math.round(config.audioSampleRate / 1000);
      
      return `${videoCodec}: ${config.videoWidth}x${config.videoHeight} @ ${config.videoFps}fps, ${config.videoBitrate}kbps, ${config.audioEncoder}: ${config.audioBitrate}kbps, ${audioSampleKHz}kHz stereo`;
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
  watch: {
    currentSource: {
      immediate: true,
      handler(newSource) {
        if (newSource) {
          this.selectedSource = newSource.toLowerCase();
        }
      }
    }
  },
  mounted() {
    if (!this.cameraConfig) {
      this.fetchSrtConfig();
    }
    // Fetch initial input source
    this.fetchInputSource();
  },
  methods: {
    async fetchSrtConfig() {
      try {
        const response = await fetch('/api/config');
        const config = await response.json();
        this.localSrtUrl = config.srtUrl;
        this.localPreviewUrl = config.previewUrl;
        this.localDroneUrl = config.droneUrl;
        this.localEncodingSettings = config.encodingSettings;
      } catch (error) {
        console.error('[StreamStats] Error fetching SRT config:', error);
        this.localSrtUrl = 'Error loading URL';
        this.localPreviewUrl = 'Error loading URL';
        this.localDroneUrl = 'Error loading URL';
        this.localEncodingSettings = null;
      }
    },
    async fetchInputSource() {
      try {
        const response = await fetch('/api/input/source');
        const data = await response.json();
        if (data.current_source) {
          this.selectedSource = data.current_source.toLowerCase();
        }
      } catch (error) {
        console.error('[StreamStats] Error fetching input source:', error);
      }
    },
    async onSourceChange() {
      this.sourceChangePending = true;
      try {
        const response = await fetch('/api/input/source', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ source: this.selectedSource })
        });
        
        if (!response.ok) {
          const data = await response.json();
          throw new Error(data.error || 'Failed to change input source');
        }
        
        console.log(`[StreamStats] Input source changed to: ${this.selectedSource}`);
      } catch (error) {
        console.error('[StreamStats] Error changing input source:', error);
        // Revert selection on error
        this.fetchInputSource();
      } finally {
        this.sourceChangePending = false;
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
    async copyPreviewToClipboard() {
      if (!this.previewUrl || this.previewUrl === 'Error loading URL') return;
      
      try {
        await navigator.clipboard.writeText(this.previewUrl);
        this.copyPreviewSuccess = true;
        setTimeout(() => {
          this.copyPreviewSuccess = false;
        }, 2000);
      } catch (error) {
        console.error('Failed to copy preview URL to clipboard:', error);
        const textArea = document.createElement('textarea');
        textArea.value = this.previewUrl;
        textArea.style.position = 'fixed';
        textArea.style.left = '-999999px';
        document.body.appendChild(textArea);
        textArea.focus();
        textArea.select();
        try {
          document.execCommand('copy');
          this.copyPreviewSuccess = true;
          setTimeout(() => {
            this.copyPreviewSuccess = false;
          }, 2000);
        } catch (err) {
          console.error('Fallback copy failed:', err);
        }
        document.body.removeChild(textArea);
      }
    },
    async copyDroneToClipboard() {
      if (!this.droneUrl || this.droneUrl === 'Error loading URL') return;
      
      try {
        await navigator.clipboard.writeText(this.droneUrl);
        this.copyDroneSuccess = true;
        setTimeout(() => {
          this.copyDroneSuccess = false;
        }, 2000);
      } catch (error) {
        console.error('Failed to copy drone URL to clipboard:', error);
        const textArea = document.createElement('textarea');
        textArea.value = this.droneUrl;
        textArea.style.position = 'fixed';
        textArea.style.left = '-999999px';
        document.body.appendChild(textArea);
        textArea.focus();
        textArea.select();
        try {
          document.execCommand('copy');
          this.copyDroneSuccess = true;
          setTimeout(() => {
            this.copyDroneSuccess = false;
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
    },
    togglePreviewUrlVisibility() {
      this.showPreviewUrl = !this.showPreviewUrl;
    },
    toggleCameraUrlVisibility() {
      this.showCameraUrl = !this.showCameraUrl;
    },
    toggleDroneUrlVisibility() {
      this.showDroneUrl = !this.showDroneUrl;
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

@media (max-width: 1024px) {
  .camera-details-grid {
    grid-template-columns: repeat(2, 1fr);
  }
}

@media (max-width: 768px) {
  .camera-details-grid {
    grid-template-columns: 1fr;
  }
}

.info-item {
  background: #0f172a;
  border-radius: 6px;
  padding: 15px;
}

.encoder-settings-item {
  grid-column: span 3;
}

.encoder-settings-value {
  font-family: 'Courier New', monospace;
  font-size: 1rem;
  color: #10b981;
  font-weight: 600;
  padding: 8px 0;
}

@media (max-width: 1024px) {
  .encoder-settings-item {
    grid-column: span 2;
  }
}

@media (max-width: 768px) {
  .encoder-settings-item {
    grid-column: span 1;
  }
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

.toggle-visibility-btn {
  background: transparent;
  border: none;
  color: #94a3b8;
  font-size: 1.1rem;
  cursor: pointer;
  padding: 8px;
  transition: color 0.2s ease, transform 0.1s ease;
  flex-shrink: 0;
}

.toggle-visibility-btn:hover {
  color: #e2e8f0;
  transform: scale(1.1);
}

.toggle-visibility-btn:active {
  transform: scale(0.95);
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

.source-select-container {
  margin-top: 8px;
}

.source-select {
  width: 100%;
  padding: 12px 16px;
  font-size: 1rem;
  font-weight: 600;
  background: #1e293b;
  color: #f1f5f9;
  border: 2px solid #334155;
  border-radius: 6px;
  cursor: pointer;
  transition: all 0.2s ease;
  appearance: none;
  background-image: url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='12' height='12' viewBox='0 0 12 12'%3E%3Cpath fill='%2394a3b8' d='M6 8L1 3h10z'/%3E%3C/svg%3E");
  background-repeat: no-repeat;
  background-position: right 12px center;
  padding-right: 36px;
}

.source-select:hover:not(:disabled) {
  border-color: #3b82f6;
}

.source-select:focus {
  outline: none;
  border-color: #3b82f6;
  box-shadow: 0 0 0 3px rgba(59, 130, 246, 0.2);
}

.source-select:disabled {
  opacity: 0.6;
  cursor: not-allowed;
}

.source-select option {
  background: #1e293b;
  color: #f1f5f9;
  padding: 12px;
}
</style>