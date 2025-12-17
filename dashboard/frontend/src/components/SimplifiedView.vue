<template>
  <div class="simplified-view">
    <div v-if="error" class="error-banner">
      <strong>Error:</strong> {{ error }}
    </div>

    <!-- Go Live Button - Only shown when stream is OFF -->
    <div v-if="!isKickLive" class="control-section go-live-section">
      <button
        @click="handleKickToggle"
        :disabled="kickActionPending"
        class="main-button button-start"
      >
        <span class="button-icon">▶</span>
        <span class="button-text">Go Live</span>
      </button>
    </div>

    <!-- Privacy Mode Banner -->
    <div v-if="isPrivacyMode" class="privacy-banner">
      <div class="privacy-icon">🔒</div>
      <div class="privacy-content">
        <h2>PRIVACY MODE ACTIVE</h2>
        <p>Your camera is currently disabled. Toggle Privacy Mode below to return to normal streaming.</p>
      </div>
    </div>

    <!-- Scene Display -->
    <div class="scene-section">
      <div class="scene-label">
        {{ isKickLive ? '🔴 LIVE ON KICK' : 'KICK OFFLINE' }}, Input = {{ displayActiveInput }}
      </div>
      <div class="scene-value">{{ displayScene }}</div>
      <div v-if="sceneDurationSeconds > 0 && displayScene !== 'Unknown'" class="scene-duration">
        {{ formatDuration(sceneDurationSeconds) }}
      </div>
    </div>

    <!-- Kick Stream Control - HIDDEN IN SIMPLIFIED VIEW -->
    <!-- Scene Control Panel - HIDDEN IN SIMPLIFIED VIEW -->

    <!-- Activate Privacy Mode Button - Only shown when Camera scene AND not in privacy mode -->
    <div v-if="isCameraScene && !isPrivacyMode" class="control-section">
      <button
        @click="handlePrivacyToggle"
        :disabled="privacyActionPending"
        class="main-button button-privacy"
      >
        <span class="button-icon">🔒</span>
        <span class="button-text">Activate Privacy Mode</span>
      </button>

      <div v-if="privacyActionPending" class="action-message">
        Activating privacy mode...
      </div>
    </div>

    <!-- End Stream Button - Only shown when stream is LIVE -->
    <div v-if="isKickLive" class="control-section">
      <button
        @click="handleKickToggle"
        :disabled="kickActionPending"
        class="main-button button-stop"
      >
        <span class="button-icon">⏹</span>
        <span class="button-text">End Stream</span>
      </button>

      <div v-if="kickActionPending" class="action-message">
        Ending stream...
      </div>
    </div>
    
    <!-- Video Preview Section -->
    <div class="preview-section">
      <VideoPreview :hlsUrl="hlsUrl" />
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
import { ref, computed, onMounted } from 'vue';
import ConfirmationModal from './ConfirmationModal.vue';
import VideoPreview from './VideoPreview.vue';

export default {
  name: 'SimplifiedView',
  components: {
    ConfirmationModal,
    VideoPreview
  },
  props: {
    containers: {
      type: Array,
      default: () => []
    },
    currentScene: {
      type: String,
      default: null
    },
    sceneDurationSeconds: {
      type: Number,
      default: 0
    },
    rtmpStats: {
      type: Object,
      default: () => ({ streams: {} })
    },
    switcherHealth: {
      type: Object,
      default: () => ({})
    },
    streamStatus: {
      type: Object,
      default: () => ({})
    }
  },
  setup(props) {
    const error = ref(null);
    const streamActionPending = ref(false);
    const privacyActionPending = ref(false);
    const kickActionPending = ref(false);
    const streamActionMessage = ref('');
    const privacyActionMessage = ref('');
    const showKickConfirmModal = ref(false);
    const pendingKickAction = ref(null);
    const kickChannelUrl = ref(null);
    const hlsUrl = ref('/api/hls/stream.m3u8');
    const activeInput = ref('camera');

    // Fetch HLS URL from config
    const fetchConfig = async () => {
      try {
        const response = await fetch('/api/config');
        const data = await response.json();
        if (data.hlsUrl) {
          hlsUrl.value = data.hlsUrl;
        }
      } catch (err) {
        console.error('[SimplifiedView] Error fetching config:', err);
      }
    };

    // Fetch active input from fallback config
    const fetchActiveInput = async () => {
      try {
        const response = await fetch('/api/fallback/config');
        const data = await response.json();
        activeInput.value = data.activeInput || 'camera';
      } catch (err) {
        console.error('[SimplifiedView] Error fetching active input:', err);
        activeInput.value = 'camera';
      }
    };

    // Computed: Display active input with capitalized first letter
    const displayActiveInput = computed(() => {
      return activeInput.value.charAt(0).toUpperCase() + activeInput.value.slice(1);
    });

    // Computed: Is Kick live
    const isKickLive = computed(() => {
      return props.switcherHealth?.kick_streaming_enabled || false;
    });

    // Computed: Is privacy mode active
    const isPrivacyMode = computed(() => {
      return props.switcherHealth?.privacy_enabled || false;
    });

    // Computed: Is current scene Camera (LIVE)
    const isCameraScene = computed(() => {
      return props.currentScene === 'LIVE';
    });

    // Computed: Display scene
    const displayScene = computed(() => {
      if (!props.currentScene) return 'Unknown';
      
      const scene = props.currentScene.toUpperCase();
      const privacyEnabled = props.switcherHealth?.privacy_enabled || false;
      
      // New scene values from multiplexer: LIVE, LIVE-CAMERA, LIVE-DRONE, FALLBACK, unknown
      if (scene === 'LIVE' || scene === 'LIVE-CAMERA') {
        return 'Camera';
      } else if (scene === 'LIVE-DRONE') {
        return 'Drone';
      } else if (scene === 'FALLBACK') {
        return privacyEnabled ? 'PRIVACY' : 'Fallback';
      } else if (scene === 'UNKNOWN') {
        return 'Unknown';
      } else if (scene === 'CONNECTING_TO_MULTIPLEXER') {
        return 'Connecting to multiplexer';
      }
      
      // Legacy support for old scene names (SRT/VIDEO/BLACK)
      if (scene === 'SRT') {
        return 'Camera';
      } else if (scene === 'VIDEO' || scene === 'BLACK') {
        return privacyEnabled ? 'PRIVACY' : 'Fallback';
      }
      
      // Return original scene name for any unknown scenes
      return props.currentScene;
    });

    // Computed: Is stream online (same as isKickLive for backwards compatibility)
    const isStreamOnline = computed(() => {
      return isKickLive.value;
    });
    
    // Computed: Kick confirmation modal properties
    const kickConfirmTitle = computed(() => {
      return pendingKickAction.value === 'start' ? 'GO LIVE ON KICK?' : 'END KICK STREAM?';
    });
    
    const kickConfirmMessage = computed(() => {
      return pendingKickAction.value === 'start'
        ? 'Are you SURE you want to GO LIVE on KICK?'
        : 'Are you SURE you want to END KICK STREAM?';
    });

    // Computed: Stream button properties
    const streamButtonText = computed(() => {
      if (streamActionPending.value) return 'Processing...';
      return isStreamOnline.value ? 'End Stream' : 'Go Live';
    });

    const streamButtonIcon = computed(() => {
      if (streamActionPending.value) return '⟳';
      return isStreamOnline.value ? '⏹' : '▶';
    });

    const streamButtonClass = computed(() => {
      if (streamActionPending.value) return 'button-processing';
      return isStreamOnline.value ? 'button-stop' : 'button-start';
    });

    // Methods
    const handleKickToggle = (event) => {
      event.preventDefault();
      pendingKickAction.value = isKickLive.value ? 'stop' : 'start';
      showKickConfirmModal.value = true;
    };
    
    const cancelKickToggle = () => {
      showKickConfirmModal.value = false;
      pendingKickAction.value = null;
    };
    
    const confirmKickToggle = async () => {
      kickActionPending.value = true;
      
      try {
        const endpoint = pendingKickAction.value === 'start'
          ? '/api/kick/start'
          : '/api/kick/stop';
        
        console.log(`[SimplifiedView] ${pendingKickAction.value === 'start' ? 'Starting' : 'Stopping'} Kick stream...`);
        
        const response = await fetch(endpoint, {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' }
        });
        
        if (!response.ok) {
          const data = await response.json();
          throw new Error(data.error || `Failed to ${pendingKickAction.value} stream`);
        }
        
        console.log(`[SimplifiedView] Kick stream ${pendingKickAction.value} successful`);
      } catch (err) {
        console.error('[SimplifiedView] Kick toggle error:', err);
        error.value = err.message;
      } finally {
        kickActionPending.value = false;
        showKickConfirmModal.value = false;
        pendingKickAction.value = null;
      }
    };
    
    const handlePrivacyToggle = () => {
      togglePrivacyMode();
    };
    
    const formatDuration = (seconds) => {
      if (seconds < 60) return `${seconds}s`;
      if (seconds < 3600) {
        const m = Math.floor(seconds / 60);
        const s = seconds % 60;
        return `${m}m ${s}s`;
      }
      const h = Math.floor(seconds / 3600);
      const m = Math.floor((seconds % 3600) / 60);
      const s = seconds % 60;
      return `${h}h ${m}m ${s}s`;
    };

    const toggleStream = async () => {
      error.value = null;
      streamActionPending.value = true;

      try {
        if (isStreamOnline.value) {
          // Stop the stream
          streamActionMessage.value = 'Ending stream...';
          const response = await fetch('/api/container/ffmpeg-kick/stop', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' }
          });

          if (!response.ok) {
            const data = await response.json();
            throw new Error(data.error || 'Failed to stop stream');
          }

          console.log('[SimplifiedView] Stream stopped successfully');
        } else {
          // Start the stream
          streamActionMessage.value = 'Starting stream...';
          const response = await fetch('/api/container/ffmpeg-kick/start', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' }
          });

          if (!response.ok) {
            const data = await response.json();
            throw new Error(data.error || 'Failed to start stream');
          }

          console.log('[SimplifiedView] Stream started successfully');
        }
      } catch (err) {
        console.error('[SimplifiedView] Stream toggle error:', err);
        error.value = err.message;
      } finally {
        streamActionPending.value = false;
        streamActionMessage.value = '';
      }
    };

    const togglePrivacyMode = async () => {
      error.value = null;
      privacyActionPending.value = true;

      try {
        if (isPrivacyMode.value) {
          // Deactivate privacy mode
          privacyActionMessage.value = 'Deactivating privacy mode...';
          
          console.log('[SimplifiedView] Disabling privacy mode...');
          const response = await fetch('/api/privacy/disable', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' }
          });

          if (!response.ok) {
            const data = await response.json();
            throw new Error(data.error || 'Failed to disable privacy mode');
          }

          console.log('[SimplifiedView] Privacy mode deactivated successfully');
        } else {
          // Activate privacy mode
          privacyActionMessage.value = 'Activating privacy mode...';

          console.log('[SimplifiedView] Enabling privacy mode...');
          const response = await fetch('/api/privacy/enable', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' }
          });

          if (!response.ok) {
            const data = await response.json();
            throw new Error(data.error || 'Failed to enable privacy mode');
          }

          console.log('[SimplifiedView] Privacy mode activated successfully');
        }
      } catch (err) {
        console.error('[SimplifiedView] Privacy mode toggle error:', err);
        error.value = err.message;
      } finally {
        privacyActionPending.value = false;
        privacyActionMessage.value = '';
      }
    };

    // Fetch Kick channel configuration
    const fetchKickChannel = async () => {
      try {
        const response = await fetch('/api/config');
        const data = await response.json();
        if (data.kickChannel) {
          kickChannelUrl.value = `https://kick.com/${data.kickChannel}`;
        }
        if (data.hlsUrl) {
          hlsUrl.value = data.hlsUrl;
        }
      } catch (err) {
        console.error('[SimplifiedView] Error fetching Kick channel:', err);
      }
    };

    // Fetch on mount
    onMounted(() => {
      fetchKickChannel();
      fetchActiveInput();
    });

    return {
      error,
      streamActionPending,
      privacyActionPending,
      kickActionPending,
      streamActionMessage,
      privacyActionMessage,
      showKickConfirmModal,
      pendingKickAction,
      isKickLive,
      isStreamOnline,
      isPrivacyMode,
      isCameraScene,
      displayScene,
      streamButtonText,
      streamButtonIcon,
      streamButtonClass,
      kickConfirmTitle,
      kickConfirmMessage,
      formatDuration,
      toggleStream,
      togglePrivacyMode,
      handleKickToggle,
      cancelKickToggle,
      confirmKickToggle,
      handlePrivacyToggle,
      kickChannelUrl,
      hlsUrl,
      activeInput,
      displayActiveInput
    };
  }
};
</script>

<style scoped>
.simplified-view {
  max-width: 800px;
  margin: 0 auto;
  padding: 40px 20px;
}

.error-banner {
  background: rgba(239, 68, 68, 0.1);
  border: 2px solid #ef4444;
  border-radius: 12px;
  padding: 20px;
  margin-bottom: 30px;
  color: #ef4444;
  font-size: 1rem;
}

.privacy-banner {
  background: linear-gradient(135deg, #ef4444 0%, #dc2626 100%);
  border-radius: 16px;
  padding: 30px;
  margin-bottom: 40px;
  display: flex;
  align-items: center;
  gap: 20px;
  box-shadow: 0 8px 16px rgba(239, 68, 68, 0.3);
  animation: pulse 2s ease-in-out infinite;
}

@keyframes pulse {
  0%, 100% {
    box-shadow: 0 8px 16px rgba(239, 68, 68, 0.3);
  }
  50% {
    box-shadow: 0 8px 24px rgba(239, 68, 68, 0.5);
  }
}

.privacy-icon {
  font-size: 3rem;
  filter: drop-shadow(0 2px 4px rgba(0, 0, 0, 0.3));
}

.privacy-content {
  flex: 1;
}

.privacy-content h2 {
  margin: 0 0 10px 0;
  font-size: 1.75rem;
  color: white;
  font-weight: bold;
  text-shadow: 0 2px 4px rgba(0, 0, 0, 0.3);
}

.privacy-content p {
  margin: 0;
  font-size: 1.1rem;
  color: rgba(255, 255, 255, 0.95);
  line-height: 1.5;
}

.control-section {
  text-align: center;
  margin-bottom: 40px;
}

.go-live-section {
  margin-bottom: 30px;
}

.control-section h1 {
  font-size: 2.5rem;
  margin: 0 0 30px 0;
  color: #f1f5f9;
  font-weight: 600;
}

.main-button {
  width: 100%;
  padding: 30px 40px;
  font-size: 2rem;
  font-weight: bold;
  border: none;
  border-radius: 16px;
  cursor: pointer;
  transition: all 0.3s ease;
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 15px;
  box-shadow: 0 8px 16px rgba(0, 0, 0, 0.2);
}

.main-button:hover:not(:disabled) {
  transform: translateY(-2px);
  box-shadow: 0 12px 24px rgba(0, 0, 0, 0.3);
}

.main-button:active:not(:disabled) {
  transform: translateY(0);
  box-shadow: 0 4px 8px rgba(0, 0, 0, 0.2);
}

.main-button:disabled {
  opacity: 0.6;
  cursor: not-allowed;
  transform: none;
}

.button-start {
  background: linear-gradient(135deg, #10b981 0%, #059669 100%);
  color: white;
}

.button-start:hover:not(:disabled) {
  background: linear-gradient(135deg, #059669 0%, #047857 100%);
}

.button-stop {
  background: linear-gradient(135deg, #ef4444 0%, #dc2626 100%);
  color: white;
}

.button-stop:hover:not(:disabled) {
  background: linear-gradient(135deg, #dc2626 0%, #b91c1c 100%);
}

.button-privacy {
  background: linear-gradient(135deg, #f59e0b 0%, #d97706 100%);
  color: white;
}

.button-privacy:hover:not(:disabled) {
  background: linear-gradient(135deg, #d97706 0%, #b45309 100%);
}

.button-processing {
  background: linear-gradient(135deg, #64748b 0%, #475569 100%);
  color: white;
}

.button-icon {
  font-size: 2.5rem;
  display: inline-block;
  animation: spin 1s linear infinite;
  animation-play-state: paused;
}

.button-processing .button-icon {
  animation-play-state: running;
}

@keyframes spin {
  to {
    transform: rotate(360deg);
  }
}

.button-text {
  font-size: 2rem;
}

.action-message {
  margin-top: 15px;
  font-size: 1.1rem;
  color: #94a3b8;
  font-style: italic;
}

.scene-section {
  background: #1e293b;
  border-radius: 12px;
  padding: 30px;
  margin-bottom: 40px;
  text-align: center;
}

.scene-label {
  font-size: 1.5rem;
  font-weight: bold;
  margin-bottom: 15px;
  text-transform: uppercase;
  letter-spacing: 1px;
}

.scene-value {
  font-size: 3rem;
  font-weight: bold;
  color: #10b981;
  margin-bottom: 10px;
}

.scene-duration {
  font-size: 1.1rem;
  color: #64748b;
}

.preview-section {
  margin-bottom: 40px;
}

.control-title {
  font-size: 1.2rem;
  color: #f1f5f9;
  margin-bottom: 20px;
  font-weight: 600;
}

.toggle-container {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 15px;
  flex-wrap: wrap;
}

.toggle-label {
  font-size: 1rem;
  color: #e2e8f0;
  flex: 1;
  min-width: 200px;
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
  gap: 10px;
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
  height: 50px;
  background: #475569;
  border-radius: 25px;
  position: relative;
  cursor: pointer;
  transition: background 0.3s ease;
  user-select: none;
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
  top: 5px;
  left: 5px;
  width: 40px;
  height: 40px;
  background: white;
  border-radius: 20px;
  transition: transform 0.3s ease;
  box-shadow: 0 2px 4px rgba(0, 0, 0, 0.2);
}

.toggle-checkbox:checked + .toggle-switch::before {
  transform: translateX(50px);
}

.toggle-text-off,
.toggle-text-on {
  position: absolute;
  top: 50%;
  transform: translateY(-50%);
  font-size: 0.75rem;
  font-weight: 600;
  color: white;
  transition: opacity 0.3s ease;
}

.toggle-text-off {
  left: 12px;
  opacity: 1;
}

.toggle-text-on {
  right: 14px;
  opacity: 0;
}

.toggle-checkbox:checked + .toggle-switch .toggle-text-off {
  opacity: 0;
}

.toggle-checkbox:checked + .toggle-switch .toggle-text-on {
  opacity: 1;
}

.toggle-status-text {
  font-size: 1rem;
  font-weight: 500;
  color: #94a3b8;
  text-align: center;
  transition: color 0.3s ease;
}

.toggle-status-text.active {
  color: #10b981;
  font-weight: 600;
}

@media (max-width: 768px) {
  .simplified-view {
    padding: 20px 15px;
  }

  .control-section h1 {
    font-size: 2rem;
  }

  .main-button {
    padding: 20px 30px;
    font-size: 1.5rem;
  }

  .button-icon {
    font-size: 2rem;
  }

  .button-text {
    font-size: 1.5rem;
  }

  .scene-value {
    font-size: 2.5rem;
  }

  .privacy-banner {
    flex-direction: column;
    text-align: center;
  }

  .privacy-content h2 {
    font-size: 1.5rem;
  }

  .privacy-content p {
    font-size: 1rem;
  }
  
  .toggle-container {
    flex-direction: column;
    align-items: stretch;
  }
  
  .toggle-label {
    text-align: center;
    margin-bottom: 10px;
  }
  
  .toggle-slider-wrapper {
    align-self: center;
  }
}
</style>