<template>
  <div class="stream-card">
    <h2>Camera Details</h2>
    
    <!-- Camera Address Section -->
    <div class="rtmp-url-section">
      <div class="url-container">
        <div class="url-display">{{ srtUrl }}</div>
        <button
          class="copy-button"
          :class="{ copied: copySuccess }"
          @click="copyToClipboard"
          :disabled="!srtUrl"
        >
          {{ copySuccess ? 'Copied!' : 'Copy' }}
        </button>
      </div>
    </div>
    
    <div class="stream-info">
      <div class="info-item">
        <div class="info-label">Stream Status</div>
        <div class="info-value status-badge" :class="isKickLive ? 'status-kick-live' : 'status-kick-offline'">
          {{ isKickLive ? '🔴 LIVE ON KICK' : 'KICK OFFLINE' }}
        </div>
        <div v-if="isKickLive" class="duration-text">
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
    
    <!-- Controls Grid -->
    <div class="controls-grid">
      <div class="control-section">
        <h3 class="section-title">Kick Stream Control</h3>
        <div class="toggle-container">
          <label class="toggle-label">
            Stream to
            <a v-if="kickChannelUrl" :href="kickChannelUrl" target="_blank" rel="noopener noreferrer" class="kick-link">
              Kick
            </a>
            <span v-else>Kick</span>
          </label>
          <div class="toggle-control-wrapper">
            <div class="toggle-slider-wrapper">
              <input
                type="checkbox"
                id="kick-toggle"
                class="toggle-checkbox"
                :checked="isKickLive"
                @click="handleKickToggle"
                :disabled="kickActionPending"
              />
              <label for="kick-toggle" class="toggle-switch">
                <span class="toggle-text-off">OFF</span>
                <span class="toggle-text-on">ON</span>
              </label>
            </div>
            <div class="toggle-status-text" :class="{ active: isKickLive }">
              {{ isKickLive ? 'Live on Kick' : 'Not Streaming' }}
            </div>
          </div>
        </div>
      </div>
      
      <div class="scene-selection-section">
        <h3 class="section-title">Scene Selection</h3>
        <div class="toggle-container">
          <label class="toggle-label">Enable Privacy Mode</label>
          <div class="toggle-control-wrapper">
            <div class="toggle-slider-wrapper">
              <input
                type="checkbox"
                id="privacy-toggle"
                class="toggle-checkbox"
                :checked="privacyEnabled"
                @change="handlePrivacyToggle"
                :disabled="settingPrivacy"
              />
              <label for="privacy-toggle" class="toggle-switch">
                <span class="toggle-text-off">Camera</span>
                <span class="toggle-text-on">Privacy</span>
              </label>
            </div>
            <div class="toggle-status-text" :class="{ active: privacyEnabled }">
              {{ privacyEnabled ? 'Privacy' : 'Camera' }}
            </div>
          </div>
        </div>
      </div>
    </div>
    
    <ConfirmationModal
      :isVisible="showKickConfirmModal"
      :title="kickConfirmTitle"
      :message="kickConfirmMessage"
      :isProcessing="kickActionPending"
      @confirm="confirmKickToggle"
      @cancel="cancelKickToggle"
    />
  </div>
</template>

<script>
import ConfirmationModal from './ConfirmationModal.vue';

export default {
  name: 'StreamStats',
  components: {
    ConfirmationModal
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
    switchingScene: {
      type: String,
      default: null
    },
    sourceAvailability: {
      type: Object,
      default: null
    },
    containers: {
      type: Array,
      default: () => []
    },
    cameraConfig: {
      type: Object,
      default: null
    },
    switcherHealth: {
      type: Object,
      default: () => ({})
    }
  },
  data() {
    return {
      switching: null,
      privacyEnabled: false,
      settingPrivacy: false,
      kickActionPending: false,
      showKickConfirmModal: false,
      pendingKickAction: null,
      kickChannelUrl: null,
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
    kickConfirmTitle() {
      return this.pendingKickAction === 'start' ? 'GO LIVE ON KICK?' : 'END KICK STREAM?';
    },
    kickConfirmMessage() {
      return this.pendingKickAction === 'start'
        ? 'Are you SURE you want to GO LIVE on KICK?'
        : 'Are you SURE you want to END KICK STREAM?';
    }
  },
  mounted() {
    this.fetchPrivacyMode();
    this.fetchKickChannel();
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
    async fetchPrivacyMode() {
      try {
        const response = await fetch('/api/privacy');
        const data = await response.json();
        this.privacyEnabled = data.enabled || false;
      } catch (error) {
        console.error('[StreamStats] Error fetching privacy mode:', error);
      }
    },
    async fetchKickChannel() {
      try {
        const response = await fetch('/api/config');
        const data = await response.json();
        if (data.kickChannel) {
          this.kickChannelUrl = `https://kick.com/${data.kickChannel}`;
        }
      } catch (error) {
        console.error('[StreamStats] Error fetching Kick channel:', error);
      }
    },
    handlePrivacyToggle(event) {
      const enabled = event.target.checked;
      this.setPrivacyMode(enabled);
    },
    async setPrivacyMode(enabled) {
      if (this.settingPrivacy) return;
      
      this.settingPrivacy = true;
      console.log(`[StreamStats] Setting privacy mode to: ${enabled}`);
      
      try {
        const endpoint = enabled ? '/api/privacy/enable' : '/api/privacy/disable';
        const response = await fetch(endpoint, {
          method: 'POST',
          headers: {
            'Content-Type': 'application/json'
          }
        });
        
        if (!response.ok) {
          const errorData = await response.json();
          throw new Error(errorData.error || 'Failed to set privacy mode');
        }
        
        const result = await response.json();
        console.log(`[StreamStats] Privacy mode set successfully:`, result);
        this.privacyEnabled = enabled;
      } catch (error) {
        console.error(`[StreamStats] Error setting privacy mode:`, error);
        alert(`Failed to set privacy mode: ${error.message}`);
      } finally {
        this.settingPrivacy = false;
      }
    },
    handleKickToggle(event) {
      event.preventDefault();
      this.pendingKickAction = this.isKickLive ? 'stop' : 'start';
      this.showKickConfirmModal = true;
    },
    cancelKickToggle() {
      this.showKickConfirmModal = false;
      this.pendingKickAction = null;
    },
    async confirmKickToggle() {
      this.kickActionPending = true;
      
      try {
        const endpoint = this.pendingKickAction === 'start'
          ? '/api/kick/start'
          : '/api/kick/stop';
        
        console.log(`[StreamStats] ${this.pendingKickAction === 'start' ? 'Starting' : 'Stopping'} Kick stream...`);
        
        const response = await fetch(endpoint, {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' }
        });
        
        if (!response.ok) {
          const data = await response.json();
          throw new Error(data.error || `Failed to ${this.pendingKickAction} stream`);
        }
        
        console.log(`[StreamStats] Kick stream ${this.pendingKickAction} successful`);
      } catch (error) {
        console.error('[StreamStats] Kick toggle error:', error);
        alert(`Failed to ${this.pendingKickAction} Kick stream: ${error.message}`);
      } finally {
        this.kickActionPending = false;
        this.showKickConfirmModal = false;
        this.pendingKickAction = null;
      }
    },
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

.rtmp-url-section {
  margin-bottom: 20px;
}

.url-container {
  display: flex;
  gap: 12px;
  align-items: stretch;
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
}

.copy-button {
  padding: 12px 24px;
  background: #3b82f6;
  color: white;
  border: none;
  border-radius: 6px;
  font-size: 0.875rem;
  font-weight: 600;
  cursor: pointer;
  transition: all 0.2s ease;
  white-space: nowrap;
  min-width: 90px;
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

.stream-info {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(250px, 1fr));
  gap: 20px;
  margin-bottom: 20px;
}

.controls-grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(250px, 1fr));
  gap: 20px;
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

.control-section,
.scene-selection-section {
  margin-top: 20px;
  background: #0f172a;
  border-radius: 6px;
  padding: 15px;
}

.section-title {
  font-size: 1rem;
  color: #f1f5f9;
  margin-bottom: 15px;
  font-weight: 600;
}

.toggle-container {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 15px;
}

.toggle-label {
  font-size: 0.9rem;
  color: #e2e8f0;
  flex: 1;
}

.kick-link {
  color: #10b981;
  text-decoration: none;
  font-weight: 600;
  border-bottom: 2px solid transparent;
  transition: border-color 0.2s ease, color 0.2s ease;
}

.kick-link:hover {
  color: #059669;
  border-bottom-color: #059669;
}

.toggle-control-wrapper {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 8px;
  flex-shrink: 0;
}

.toggle-slider-wrapper {
  position: relative;
}

.toggle-checkbox {
  display: none;
}

.toggle-switch {
  display: block;
  width: 100px;
  height: 40px;
  background: #475569;
  border-radius: 20px;
  position: relative;
  cursor: pointer;
  transition: background 0.3s ease;
  user-select: none;
}

.toggle-switch span {
  display: none;
}

.toggle-checkbox:checked + .toggle-switch {
  background: #10b981;
}

.toggle-checkbox:disabled + .toggle-switch {
  opacity: 0.5;
  cursor: not-allowed;
}

.toggle-switch::before {
  content: '';
  position: absolute;
  top: 4px;
  left: 4px;
  width: 32px;
  height: 32px;
  background: white;
  border-radius: 16px;
  transition: transform 0.3s ease;
  box-shadow: 0 2px 4px rgba(0, 0, 0, 0.2);
}

.toggle-checkbox:checked + .toggle-switch::before {
  transform: translateX(60px);
}

.toggle-text-off,
.toggle-text-on {
  position: absolute;
  top: 50%;
  transform: translateY(-50%);
  font-size: 0.7rem;
  font-weight: 600;
  color: white;
  transition: opacity 0.3s ease;
}

.toggle-text-off {
  left: 10px;
  opacity: 1;
}

.toggle-text-on {
  right: 12px;
  opacity: 0;
}

.toggle-checkbox:checked + .toggle-switch .toggle-text-off {
  opacity: 0;
}

.toggle-checkbox:checked + .toggle-switch .toggle-text-on {
  opacity: 1;
}

.toggle-status-text {
  font-size: 0.875rem;
  font-weight: 500;
  color: #94a3b8;
  text-align: center;
  transition: color 0.3s ease;
}

.toggle-status-text.active {
  color: #10b981;
  font-weight: 600;
}
</style>