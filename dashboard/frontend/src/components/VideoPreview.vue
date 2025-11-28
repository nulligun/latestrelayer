<template>
  <div class="video-preview-container">
    <div class="video-preview-header">
      <h3 class="preview-title">Stream Preview</h3>
      <span v-if="isPlaying" class="live-indicator">● STAND BY</span>
    </div>
    
    <div class="video-wrapper" @click="togglePlayback">
      <!-- Poster overlay when not playing -->
      <div v-if="!isPlaying && !isLoading" class="poster-overlay">
        <div class="poster-content">
          <div class="play-icon">▶</div>
          <p class="poster-text">Tap to preview stream</p>
        </div>
      </div>
      
      <!-- Loading indicator -->
      <div v-if="isLoading" class="loading-overlay">
        <div class="loading-spinner"></div>
        <p class="loading-text">Loading stream...</p>
      </div>
      
      <!-- Error overlay -->
      <div v-if="error && !isPlaying" class="error-overlay">
        <div class="error-icon">⚠</div>
        <p class="error-text">{{ error }}</p>
        <button @click.stop="retryPlayback" class="retry-button">Retry</button>
      </div>
      
      <!-- Video element -->
      <video
        ref="videoElement"
        class="video-player"
        :muted="isMuted"
        playsinline
        @ended="handleEnded"
        @error="handleVideoError"
      ></video>
      
      <!-- Mute indicator when playing -->
      <div v-if="isPlaying" class="mute-indicator" @click.stop="toggleMute">
        {{ isMuted ? '🔇 Tap to unmute' : '🔊' }}
      </div>
    </div>
  </div>
</template>

<script>
import { ref, onMounted, onUnmounted } from 'vue';
import Hls from 'hls.js';

export default {
  name: 'VideoPreview',
  props: {
    hlsUrl: {
      type: String,
      default: '/api/hls/test.m3u8'
    }
  },
  setup(props) {
    const videoElement = ref(null);
    const isPlaying = ref(false);
    const isLoading = ref(false);
    const isMuted = ref(true);
    const error = ref(null);
    let hls = null;

    const initHls = () => {
      if (!videoElement.value) return;

      // Clean up existing HLS instance
      destroyHls();

      if (Hls.isSupported()) {
        hls = new Hls({
          enableWorker: true,
          lowLatencyMode: true,
          backBufferLength: 30
        });

        hls.loadSource(props.hlsUrl);
        hls.attachMedia(videoElement.value);

        hls.on(Hls.Events.MANIFEST_PARSED, () => {
          console.log('[VideoPreview] HLS manifest parsed, starting playback');
          isLoading.value = false;
          error.value = null;
          videoElement.value.play()
            .then(() => {
              isPlaying.value = true;
            })
            .catch(err => {
              console.error('[VideoPreview] Autoplay failed:', err);
              error.value = 'Tap to play';
              isLoading.value = false;
            });
        });

        hls.on(Hls.Events.ERROR, (event, data) => {
          console.error('[VideoPreview] HLS error:', data);
          if (data.fatal) {
            isLoading.value = false;
            switch (data.type) {
              case Hls.ErrorTypes.NETWORK_ERROR:
                error.value = 'Stream not available';
                break;
              case Hls.ErrorTypes.MEDIA_ERROR:
                error.value = 'Media error';
                hls.recoverMediaError();
                break;
              default:
                error.value = 'Stream error';
                destroyHls();
                break;
            }
          }
        });
      } else if (videoElement.value.canPlayType('application/vnd.apple.mpegurl')) {
        // Safari native HLS support
        videoElement.value.src = props.hlsUrl;
        videoElement.value.addEventListener('loadedmetadata', () => {
          isLoading.value = false;
          error.value = null;
          videoElement.value.play()
            .then(() => {
              isPlaying.value = true;
            })
            .catch(err => {
              console.error('[VideoPreview] Safari autoplay failed:', err);
              error.value = 'Tap to play';
              isLoading.value = false;
            });
        });
      } else {
        error.value = 'HLS not supported';
        isLoading.value = false;
      }
    };

    const destroyHls = () => {
      if (hls) {
        hls.destroy();
        hls = null;
      }
    };

    const togglePlayback = () => {
      if (isLoading.value) return;

      if (isPlaying.value) {
        // Pause playback
        videoElement.value?.pause();
        isPlaying.value = false;
        destroyHls();
      } else {
        // Start playback
        isLoading.value = true;
        error.value = null;
        initHls();
      }
    };

    const toggleMute = () => {
      isMuted.value = !isMuted.value;
      if (videoElement.value) {
        videoElement.value.muted = isMuted.value;
      }
    };

    const retryPlayback = () => {
      error.value = null;
      isLoading.value = true;
      initHls();
    };

    const handleEnded = () => {
      isPlaying.value = false;
    };

    const handleVideoError = (e) => {
      console.error('[VideoPreview] Video error:', e);
      if (!error.value) {
        error.value = 'Video playback error';
      }
      isLoading.value = false;
      isPlaying.value = false;
    };

    onUnmounted(() => {
      destroyHls();
    });

    return {
      videoElement,
      isPlaying,
      isLoading,
      isMuted,
      error,
      togglePlayback,
      toggleMute,
      retryPlayback,
      handleEnded,
      handleVideoError
    };
  }
};
</script>

<style scoped>
.video-preview-container {
  background: #1e293b;
  border-radius: 12px;
  overflow: hidden;
  margin-top: 20px;
}

.video-preview-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 15px 20px;
  border-bottom: 1px solid #334155;
}

.preview-title {
  margin: 0;
  font-size: 1.1rem;
  color: #f1f5f9;
  font-weight: 600;
}

.live-indicator {
  color: #ef4444;
  font-size: 0.85rem;
  font-weight: 600;
  animation: pulse 2s ease-in-out infinite;
}

@keyframes pulse {
  0%, 100% { opacity: 1; }
  50% { opacity: 0.5; }
}

.video-wrapper {
  position: relative;
  width: 100%;
  padding-bottom: 56.25%; /* 16:9 aspect ratio */
  background: #0f172a;
  cursor: pointer;
}

.video-player {
  position: absolute;
  top: 0;
  left: 0;
  width: 100%;
  height: 100%;
  object-fit: contain;
}

.poster-overlay,
.loading-overlay,
.error-overlay {
  position: absolute;
  top: 0;
  left: 0;
  width: 100%;
  height: 100%;
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  background: rgba(15, 23, 42, 0.9);
  z-index: 10;
}

.poster-content {
  text-align: center;
}

.play-icon {
  font-size: 4rem;
  color: #10b981;
  margin-bottom: 15px;
  transition: transform 0.2s ease;
}

.video-wrapper:hover .play-icon {
  transform: scale(1.1);
}

.poster-text {
  color: #94a3b8;
  font-size: 1rem;
  margin: 0;
}

.loading-spinner {
  width: 50px;
  height: 50px;
  border: 4px solid #334155;
  border-top-color: #10b981;
  border-radius: 50%;
  animation: spin 1s linear infinite;
  margin-bottom: 15px;
}

@keyframes spin {
  to { transform: rotate(360deg); }
}

.loading-text {
  color: #94a3b8;
  font-size: 1rem;
  margin: 0;
}

.error-icon {
  font-size: 3rem;
  color: #f59e0b;
  margin-bottom: 15px;
}

.error-text {
  color: #ef4444;
  font-size: 1rem;
  margin: 0 0 15px 0;
}

.retry-button {
  background: #10b981;
  color: white;
  border: none;
  padding: 10px 25px;
  border-radius: 8px;
  font-size: 1rem;
  cursor: pointer;
  transition: background 0.2s ease;
}

.retry-button:hover {
  background: #059669;
}

.mute-indicator {
  position: absolute;
  bottom: 15px;
  right: 15px;
  background: rgba(0, 0, 0, 0.7);
  color: white;
  padding: 8px 12px;
  border-radius: 6px;
  font-size: 0.85rem;
  cursor: pointer;
  transition: background 0.2s ease;
  z-index: 5;
}

.mute-indicator:hover {
  background: rgba(0, 0, 0, 0.9);
}

@media (max-width: 768px) {
  .video-preview-header {
    padding: 12px 15px;
  }
  
  .preview-title {
    font-size: 1rem;
  }
  
  .play-icon {
    font-size: 3rem;
  }
  
  .poster-text,
  .loading-text,
  .error-text {
    font-size: 0.9rem;
  }
}
</style>