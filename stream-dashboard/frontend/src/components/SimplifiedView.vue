<template>
  <div class="simplified-view">
    <div v-if="error" class="error-banner">
      <strong>Error:</strong> {{ error }}
    </div>

    <!-- Privacy Mode Banner -->
    <div v-if="isPrivacyMode" class="privacy-banner">
      <div class="privacy-icon">🔒</div>
      <div class="privacy-content">
        <h2>PRIVACY MODE ACTIVE</h2>
        <p>Your camera is currently disabled. Click "Deactivate Privacy Mode" below to return to normal streaming.</p>
      </div>
    </div>

    <!-- Scene Display -->
    <div class="scene-section">
      <div class="scene-label">Current Scene</div>
      <div class="scene-value">{{ displayScene }}</div>
      <div v-if="sceneDurationSeconds > 0" class="scene-duration">
        {{ formatDuration(sceneDurationSeconds) }}
      </div>
    </div>

    <!-- Privacy Mode Control -->
    <div class="privacy-section">
      <button 
        @click="togglePrivacyMode"
        :disabled="privacyActionPending"
        class="privacy-button"
        :class="{ 'privacy-active': isPrivacyMode }"
      >
        <span class="button-icon">{{ privacyButtonIcon }}</span>
        <span class="button-text">{{ privacyButtonText }}</span>
      </button>

      <div v-if="privacyActionPending" class="action-message">
        {{ privacyActionMessage }}
      </div>
    </div>

    <!-- Main Stream Control -->
    <div class="control-section">
      <button 
        @click="toggleStream"
        :disabled="streamActionPending"
        class="main-button"
        :class="streamButtonClass"
      >
        <span class="button-icon">{{ streamButtonIcon }}</span>
        <span class="button-text">{{ streamButtonText }}</span>
      </button>

      <div v-if="streamActionPending" class="action-message">
        {{ streamActionMessage }}
      </div>
    </div>
  </div>
</template>

<script>
import { ref, computed, watch } from 'vue';

export default {
  name: 'SimplifiedView',
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
    }
  },
  setup(props) {
    const error = ref(null);
    const streamActionPending = ref(false);
    const privacyActionPending = ref(false);
    const streamActionMessage = ref('');
    const privacyActionMessage = ref('');

    // Computed: Find ffmpeg-kick container
    const kickContainer = computed(() => {
      return props.containers.find(c => c.name === 'ffmpeg-kick');
    });

    // Computed: Find stream-auto-switcher container
    const autoSwitcherContainer = computed(() => {
      return props.containers.find(c => c.name === 'stream-auto-switcher');
    });

    // Computed: Is stream online
    const isStreamOnline = computed(() => {
      return kickContainer.value?.running || false;
    });

    // Computed: Is privacy mode active (auto-switcher stopped AND scene is BRB)
    const isPrivacyMode = computed(() => {
      const autoSwitcherStopped = !autoSwitcherContainer.value?.running;
      const sceneIsBRB = props.currentScene === 'brb';
      return autoSwitcherStopped && sceneIsBRB;
    });

    // Computed: Display scene
    const displayScene = computed(() => {
      if (!props.currentScene) return 'UNKNOWN';
      if (props.currentScene === 'cam-raw' || props.currentScene === 'cam') {
        return 'Camera';
      }
      return props.currentScene.toUpperCase();
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

    // Computed: Privacy button properties
    const privacyButtonText = computed(() => {
      if (privacyActionPending.value) return 'Processing...';
      return isPrivacyMode.value ? 'Deactivate Privacy Mode' : 'Activate Privacy Mode';
    });

    const privacyButtonIcon = computed(() => {
      if (privacyActionPending.value) return '⟳';
      return isPrivacyMode.value ? '📹' : '🔒';
    });

    // Methods
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
          privacyActionMessage.value = 'Activating camera...';

          // Start auto-switcher
          console.log('[SimplifiedView] Starting auto-switcher...');
          const switcherResponse = await fetch('/api/container/stream-auto-switcher/start', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' }
          });

          if (!switcherResponse.ok) {
            const data = await switcherResponse.json();
            throw new Error(data.error || 'Failed to start auto-switcher');
          }

          // Wait a moment for auto-switcher to start
          await new Promise(resolve => setTimeout(resolve, 1000));

          // Switch to camera scene (prefer cam-raw, fallback to cam)
          const targetScene = props.rtmpStats.streams['cam-raw'] ? 'cam-raw' : 'cam';
          console.log(`[SimplifiedView] Switching to ${targetScene}...`);
          
          const sceneResponse = await fetch('/api/scene/switch', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ scene: targetScene })
          });

          if (!sceneResponse.ok) {
            const data = await sceneResponse.json();
            throw new Error(data.error || 'Failed to switch scene');
          }

          console.log('[SimplifiedView] Camera activated successfully');
        } else {
          // Activate privacy mode
          privacyActionMessage.value = 'Activating privacy mode...';

          // Switch to BRB scene first
          console.log('[SimplifiedView] Switching to BRB...');
          const sceneResponse = await fetch('/api/scene/switch', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ scene: 'brb' })
          });

          if (!sceneResponse.ok) {
            const data = await sceneResponse.json();
            throw new Error(data.error || 'Failed to switch to BRB');
          }

          // Wait a moment for scene to switch
          await new Promise(resolve => setTimeout(resolve, 500));

          // Stop auto-switcher
          console.log('[SimplifiedView] Stopping auto-switcher...');
          const switcherResponse = await fetch('/api/container/stream-auto-switcher/stop', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' }
          });

          if (!switcherResponse.ok) {
            const data = await switcherResponse.json();
            throw new Error(data.error || 'Failed to stop auto-switcher');
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

    return {
      error,
      streamActionPending,
      privacyActionPending,
      streamActionMessage,
      privacyActionMessage,
      isStreamOnline,
      isPrivacyMode,
      displayScene,
      streamButtonText,
      streamButtonIcon,
      streamButtonClass,
      privacyButtonText,
      privacyButtonIcon,
      formatDuration,
      toggleStream,
      togglePrivacyMode
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
  font-size: 1.2rem;
  color: #94a3b8;
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

.privacy-section {
  text-align: center;
  margin-bottom: 40px;
}

.privacy-button {
  width: 100%;
  padding: 20px 30px;
  font-size: 1.5rem;
  font-weight: bold;
  border: 2px solid #64748b;
  border-radius: 12px;
  cursor: pointer;
  transition: all 0.3s ease;
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 12px;
  background: #1e293b;
  color: #e2e8f0;
  box-shadow: 0 4px 8px rgba(0, 0, 0, 0.1);
}

.privacy-button:hover:not(:disabled) {
  transform: translateY(-2px);
  box-shadow: 0 8px 16px rgba(0, 0, 0, 0.2);
  border-color: #94a3b8;
}

.privacy-button:active:not(:disabled) {
  transform: translateY(0);
  box-shadow: 0 2px 4px rgba(0, 0, 0, 0.1);
}

.privacy-button:disabled {
  opacity: 0.6;
  cursor: not-allowed;
  transform: none;
}

.privacy-button.privacy-active {
  background: linear-gradient(135deg, #10b981 0%, #059669 100%);
  color: white;
  border-color: #10b981;
}

.privacy-button.privacy-active:hover:not(:disabled) {
  background: linear-gradient(135deg, #059669 0%, #047857 100%);
  border-color: #059669;
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
}
</style>