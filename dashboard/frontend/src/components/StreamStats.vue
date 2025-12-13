<template>
  <div class="stream-card">
    <h2>Camera Details</h2>
    
    <!-- Camera Details - 2 Column Layout -->
    <div class="camera-details-grid">
      <!-- Column 1: URLs -->
      <div class="url-column">
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
      </div>
      
      <!-- Column 2: Status and Controls -->
      <div class="status-column">
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
          <div v-if="displayScene !== 'Unknown'" class="duration-text">
            {{ formatDuration(sceneDurationSeconds) }}
          </div>
        </div>
        
        <!-- Active Input -->
        <div class="info-item">
          <div class="info-label">Active Input</div>
          <div class="active-input-select-group">
            <v-select
              v-model="localActiveInput"
              :options="activeInputOptions"
              :reduce="option => option.value"
              :disabled="updatingActiveInput"
              :clearable="false"
              :searchable="false"
              @option:selected="handleActiveInputChange"
              class="active-input-select-vue"
              placeholder="Select active input"
            />
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<script>
import vSelect from 'vue-select';
import 'vue-select/dist/vue-select.css';

export default {
  name: 'StreamStats',
  components: {
    vSelect
  },
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
      localPreviewUrl: '',
      localDroneUrl: '',
      copySuccess: false,
      copyPreviewSuccess: false,
      copyDroneSuccess: false,
      showPreviewUrl: false,
      showCameraUrl: false,
      showDroneUrl: false,
      localActiveInput: 'camera',
      updatingActiveInput: false,
      activeInputOptions: [
        { label: 'Camera', value: 'camera' },
        { label: 'Drone', value: 'drone' }
      ]
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
        return 'Unknown';
      } else if (scene === 'CONNECTING_TO_MULTIPLEXER') {
        return 'Connecting to multiplexer';
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
    this.fetchActiveInput();
  },
  methods: {
    async fetchSrtConfig() {
      try {
        const response = await fetch('/api/config');
        const config = await response.json();
        this.localSrtUrl = config.srtUrl;
        this.localPreviewUrl = config.previewUrl;
        this.localDroneUrl = config.droneUrl;
      } catch (error) {
        console.error('[StreamStats] Error fetching SRT config:', error);
        this.localSrtUrl = 'Error loading URL';
        this.localPreviewUrl = 'Error loading URL';
        this.localDroneUrl = 'Error loading URL';
      }
    },
    async fetchActiveInput() {
      try {
        const response = await fetch('/api/fallback/config');
        const config = await response.json();
        this.localActiveInput = config.activeInput || 'camera';
      } catch (error) {
        console.error('[StreamStats] Error fetching active input:', error);
        this.localActiveInput = 'camera';
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
    },
    async handleActiveInputChange(value) {
      const newValue = value?.value || value;
      this.localActiveInput = newValue;
      if (this.updatingActiveInput) return;
      
      this.updatingActiveInput = true;
      
      try {
        const response = await fetch('/api/fallback/config', {
          method: 'POST',
          headers: {
            'Content-Type': 'application/json'
          },
          body: JSON.stringify({
            activeInput: newValue
          })
        });
        
        if (!response.ok) {
          const errorData = await response.json();
          throw new Error(errorData.error || 'Failed to update active input');
        }
        
        const result = await response.json();
        console.log('[StreamStats] Active input updated:', result);
      } catch (error) {
        console.error('[StreamStats] Error updating active input:', error);
        alert(`Failed to update active input: ${error.message}`);
      } finally {
        this.updatingActiveInput = false;
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
  grid-template-columns: repeat(2, 1fr);
  gap: 20px;
}

.url-column,
.status-column {
  display: flex;
  flex-direction: column;
  gap: 15px;
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

/* Active Input Select Styling */
.active-input-select-group {
  margin-top: 8px;
}

.active-input-select-vue {
  width: 100%;
  position: relative;
}

/* Vue-select custom dark theme styling for active input */
.active-input-select-vue :deep(.vs__dropdown-toggle) {
  background: #1e293b;
  border: 1px solid #334155;
  border-radius: 6px;
  padding: 6px 8px;
  transition: border-color 0.2s ease;
}

.active-input-select-vue :deep(.vs__dropdown-toggle:hover) {
  border-color: #3b82f6;
}

.active-input-select-vue :deep(.vs__selected) {
  color: #e2e8f0;
  font-size: 0.9rem;
  margin: 2px;
  padding: 2px 6px;
}

.active-input-select-vue :deep(.vs__search),
.active-input-select-vue :deep(.vs__search:focus) {
  color: #e2e8f0;
  margin: 2px 0;
  padding: 2px;
}

.active-input-select-vue :deep(.vs__search::placeholder) {
  color: #64748b;
}

.active-input-select-vue :deep(.vs__actions) {
  padding: 2px 6px;
}

.active-input-select-vue :deep(.vs__open-indicator) {
  fill: #94a3b8;
  transition: transform 0.2s ease;
}

.active-input-select-vue :deep(.vs__dropdown-toggle:hover .vs__open-indicator) {
  fill: #3b82f6;
}

.active-input-select-vue :deep(.vs__clear) {
  fill: #94a3b8;
  transition: fill 0.2s ease;
}

.active-input-select-vue :deep(.vs__clear:hover) {
  fill: #ef4444;
}

.active-input-select-vue :deep(.vs__dropdown-menu) {
  background: #1e293b;
  border: 1px solid #334155;
  border-radius: 6px;
  margin-top: 4px;
  padding: 4px 0;
  box-shadow: 0 4px 6px rgba(0, 0, 0, 0.3);
}

.active-input-select-vue :deep(.vs__dropdown-option) {
  color: #e2e8f0;
  padding: 8px 12px;
  transition: background-color 0.15s ease, color 0.15s ease;
}

.active-input-select-vue :deep(.vs__dropdown-option--highlight) {
  background: #3b82f6;
  color: #ffffff;
}

.active-input-select-vue :deep(.vs__dropdown-option--selected) {
  background: rgba(59, 130, 246, 0.2);
  color: #ffffff;
  font-weight: 600;
}

.active-input-select-vue :deep(.vs__dropdown-option--disabled) {
  color: #64748b;
  cursor: not-allowed;
}

.active-input-select-vue :deep(.vs__no-options) {
  color: #94a3b8;
  padding: 12px;
  text-align: center;
}

.active-input-select-vue :deep(.vs__spinner) {
  border-left-color: #3b82f6;
}

/* Disabled state */
.active-input-select-vue :deep(.vs--disabled .vs__dropdown-toggle) {
  background: #0f172a;
  opacity: 0.6;
  cursor: not-allowed;
}

.active-input-select-vue :deep(.vs--disabled .vs__selected) {
  color: #64748b;
}

.active-input-select-vue :deep(.vs--disabled .vs__open-indicator) {
  fill: #64748b;
}
</style>