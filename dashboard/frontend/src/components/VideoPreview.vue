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
      
      <!-- Recovery indicator -->
      <div v-if="isRecovering" class="recovery-overlay">
        <div class="loading-spinner"></div>
        <p class="recovery-text">Recovering stream...</p>
        <p class="recovery-subtext">Attempt {{ recoveryAttempts }} of {{ maxRecoveryAttempts }}</p>
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
      default: '/live/stream.m3u8'
    }
  },
  setup(props) {
    const videoElement = ref(null);
    const isPlaying = ref(false);
    const isLoading = ref(false);
    const isPlayPending = ref(false);
    const isMuted = ref(true);
    const error = ref(null);
    const isRecovering = ref(false);
    const recoveryAttempts = ref(0);
    const maxRecoveryAttempts = ref(3);
    let hls = null;
    let mediaErrorRecoveryCount = 0;

    const initHls = () => {
      if (!videoElement.value) return;

      // Clean up existing HLS instance
      destroyHls();
      
      // Reset recovery counter on fresh init
      mediaErrorRecoveryCount = 0;

      if (Hls.isSupported()) {
        hls = new Hls({
          enableWorker: true,
          lowLatencyMode: true,
          backBufferLength: 30,
          maxBufferHole: 0.5,           // Tolerate small gaps in buffer
          maxMaxBufferLength: 30,
          fragLoadingMaxRetry: 6,       // More fragment retries
          manifestLoadingMaxRetry: 4,
          levelLoadingMaxRetry: 4,
          appendErrorMaxRetry: 3,       // Retry buffer appends
          debug: true  // Enable HLS.js debug logging
        });

        hls.loadSource(props.hlsUrl);
        hls.attachMedia(videoElement.value);

        // Enhanced debugging: Log all HLS events for diagnosis
        hls.on(Hls.Events.LEVEL_LOADED, (event, data) => {
          console.log('[VideoPreview] Level loaded:', {
            level: data.level,
            details: data.details,
            totalduration: data.details?.totalduration,
            fragments: data.details?.fragments?.length,
            type: data.details?.type
          });
          // Log codec info from the level
          if (hls.levels && hls.levels[data.level]) {
            const level = hls.levels[data.level];
            console.log('[VideoPreview] Level codec info:', {
              videoCodec: level.videoCodec,
              audioCodec: level.audioCodec,
              width: level.width,
              height: level.height,
              bitrate: level.bitrate,
              attrs: level.attrs
            });
          }
        });

        hls.on(Hls.Events.FRAG_LOADED, (event, data) => {
          console.log('[VideoPreview] Fragment loaded:', {
            sn: data.frag.sn,
            cc: data.frag.cc,  // Continuity counter - changes on discontinuity
            duration: data.frag.duration,
            start: data.frag.start,
            level: data.frag.level
          });
        });

        hls.on(Hls.Events.FRAG_PARSING_INIT_SEGMENT, (event, data) => {
          console.log('[VideoPreview] Init segment parsed:', {
            videoCodec: data.videoCodec,
            audioCodec: data.audioCodec,
            tracks: data.tracks
          });
        });

        hls.on(Hls.Events.BUFFER_CODECS, (event, data) => {
          console.log('[VideoPreview] Buffer codecs:', {
            video: data.video,
            audio: data.audio
          });
        });

        hls.on(Hls.Events.MANIFEST_PARSED, () => {
          console.log('[VideoPreview] HLS manifest parsed, starting playback');
          // Log available levels/qualities
          if (hls.levels) {
            console.log('[VideoPreview] Available levels:', hls.levels.map((level, i) => ({
              index: i,
              width: level.width,
              height: level.height,
              videoCodec: level.videoCodec,
              audioCodec: level.audioCodec,
              bitrate: level.bitrate
            })));
          }
          isLoading.value = false;
          error.value = null;
          isPlayPending.value = true;
          videoElement.value.play()
            .then(() => {
              isPlayPending.value = false;
              isPlaying.value = true;
            })
            .catch(err => {
              isPlayPending.value = false;
              // AbortError is expected when playback is interrupted intentionally
              if (err.name === 'AbortError') {
                console.log('[VideoPreview] Play request was interrupted (expected during stop)');
                return;
              }
              console.error('[VideoPreview] Autoplay failed:', err);
              error.value = 'Tap to play';
              isLoading.value = false;
            });
        });

        hls.on(Hls.Events.ERROR, (event, data) => {
          // Enhanced error logging with all available details
          console.error('[VideoPreview] HLS error:', {
            type: data.type,
            details: data.details,
            fatal: data.fatal,
            reason: data.reason,
            frag: data.frag ? {
              sn: data.frag.sn,
              cc: data.frag.cc,
              level: data.frag.level,
              url: data.frag.url
            } : null,
            level: data.level,
            url: data.url,
            response: data.response ? {
              code: data.response.code,
              text: data.response.text
            } : null,
            error: data.error,
            event: data.event
          });
          
          // If it's a media error, log additional video element state
          if (data.type === Hls.ErrorTypes.MEDIA_ERROR) {
            const video = videoElement.value;
            if (video) {
              console.error('[VideoPreview] Video element state during media error:', {
                readyState: video.readyState,
                networkState: video.networkState,
                error: video.error ? {
                  code: video.error.code,
                  message: video.error.message,
                  // MediaError codes: 1=ABORTED, 2=NETWORK, 3=DECODE, 4=SRC_NOT_SUPPORTED
                  codeExplained: ['', 'MEDIA_ERR_ABORTED', 'MEDIA_ERR_NETWORK', 'MEDIA_ERR_DECODE', 'MEDIA_ERR_SRC_NOT_SUPPORTED'][video.error.code]
                } : null,
                currentTime: video.currentTime,
                buffered: video.buffered.length > 0 ? {
                  start: video.buffered.start(0),
                  end: video.buffered.end(video.buffered.length - 1)
                } : 'empty'
              });
            }
          }

          // Handle non-fatal fragment errors - skip corrupted segments
          if (!data.fatal && data.type === Hls.ErrorTypes.MEDIA_ERROR) {
            if (data.details === Hls.ErrorDetails.FRAG_PARSING_ERROR || 
                data.details === Hls.ErrorDetails.FRAG_LOAD_ERROR) {
              console.warn('[VideoPreview] Non-fatal fragment error - HLS.js will skip and continue');
              return;
            }
          }

          if (data.fatal) {
            switch (data.type) {
              case Hls.ErrorTypes.NETWORK_ERROR:
                console.error('[VideoPreview] Fatal network error - possible causes: stream not running, CORS issue, or segment fetch failed');
                // Try to recover from network errors
                if (mediaErrorRecoveryCount < maxRecoveryAttempts.value) {
                  mediaErrorRecoveryCount++;
                  isRecovering.value = true;
                  console.log(`[VideoPreview] Attempting network recovery (${mediaErrorRecoveryCount}/${maxRecoveryAttempts.value})`);
                  setTimeout(() => {
                    if (hls) {
                      hls.startLoad();
                      isRecovering.value = false;
                    }
                  }, 1000);
                } else {
                  isLoading.value = false;
                  error.value = `Stream not available (${data.details})`;
                }
                break;
                
              case Hls.ErrorTypes.MEDIA_ERROR:
                console.error('[VideoPreview] Fatal media error - attempting progressive recovery');
                handleMediaErrorRecovery(data);
                break;
                
              default:
                error.value = `Stream error: ${data.details}`;
                console.error('[VideoPreview] Fatal unknown error - destroying HLS instance');
                isLoading.value = false;
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
          isPlayPending.value = true;
          videoElement.value.play()
            .then(() => {
              isPlayPending.value = false;
              isPlaying.value = true;
            })
            .catch(err => {
              isPlayPending.value = false;
              // AbortError is expected when playback is interrupted intentionally
              if (err.name === 'AbortError') {
                console.log('[VideoPreview] Safari play request was interrupted (expected during stop)');
                return;
              }
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

    const handleMediaErrorRecovery = (data) => {
      if (mediaErrorRecoveryCount >= maxRecoveryAttempts.value) {
        console.error('[VideoPreview] Max recovery attempts reached - giving up');
        isLoading.value = false;
        isRecovering.value = false;
        error.value = `Media error: ${data.details} (recovery failed)`;
        return;
      }

      mediaErrorRecoveryCount++;
      recoveryAttempts.value = mediaErrorRecoveryCount;
      isRecovering.value = true;
      
      console.log(`[VideoPreview] Media error recovery attempt ${mediaErrorRecoveryCount}/${maxRecoveryAttempts.value}`);

      // Progressive recovery strategy
      if (mediaErrorRecoveryCount === 1) {
        // First attempt: Simple media error recovery
        console.log('[VideoPreview] Recovery strategy 1: recoverMediaError()');
        setTimeout(() => {
          if (hls) {
            hls.recoverMediaError();
            isRecovering.value = false;
          }
        }, 500);
      } else if (mediaErrorRecoveryCount === 2) {
        // Second attempt: Swap audio codec and recover
        console.log('[VideoPreview] Recovery strategy 2: swapAudioCodec() + recoverMediaError()');
        setTimeout(() => {
          if (hls) {
            hls.swapAudioCodec();
            hls.recoverMediaError();
            isRecovering.value = false;
          }
        }, 500);
      } else {
        // Third attempt: Full reinitialize
        console.log('[VideoPreview] Recovery strategy 3: Full HLS reinitialization');
        setTimeout(() => {
          isRecovering.value = false;
          isLoading.value = true;
          initHls();
        }, 1000);
      }
    };

    const destroyHls = () => {
      if (hls) {
        hls.destroy();
        hls = null;
      }
      mediaErrorRecoveryCount = 0;
      recoveryAttempts.value = 0;
      isRecovering.value = false;
    };

    const togglePlayback = () => {
      // Prevent re-entry while loading or while a play operation is pending
      if (isLoading.value || isPlayPending.value) return;

      if (isPlaying.value) {
        // Pause playback
        videoElement.value?.pause();
        isPlaying.value = false;
        destroyHls();
      } else {
        // Start playback
        isLoading.value = true;
        error.value = null;
        recoveryAttempts.value = 0;
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
      recoveryAttempts.value = 0;
      mediaErrorRecoveryCount = 0;
      initHls();
    };

    const handleEnded = () => {
      isPlaying.value = false;
    };

    const handleVideoError = (e) => {
      // Enhanced video element error logging
      console.error('[VideoPreview] Video element error event:', e);
      const video = videoElement.value;
      if (video && video.error) {
        const mediaErrorCodes = ['', 'MEDIA_ERR_ABORTED', 'MEDIA_ERR_NETWORK', 'MEDIA_ERR_DECODE', 'MEDIA_ERR_SRC_NOT_SUPPORTED'];
        console.error('[VideoPreview] Video MediaError details:', {
          code: video.error.code,
          codeExplained: mediaErrorCodes[video.error.code] || 'UNKNOWN',
          message: video.error.message || '(no message)',
          readyState: video.readyState,
          networkState: video.networkState,
          currentSrc: video.currentSrc,
          buffered: video.buffered.length > 0 ? `${video.buffered.start(0)}-${video.buffered.end(video.buffered.length - 1)}` : 'empty'
        });
      }
      // Don't reset state if a play operation is pending - the AbortError handler will manage cleanup
      if (isPlayPending.value) {
        console.log('[VideoPreview] Video error during pending play operation - deferring to play Promise handler');
        return;
      }
      if (!error.value) {
        const video = videoElement.value;
        if (video && video.error) {
          const mediaErrorCodes = ['', 'MEDIA_ERR_ABORTED', 'MEDIA_ERR_NETWORK', 'MEDIA_ERR_DECODE', 'MEDIA_ERR_SRC_NOT_SUPPORTED'];
          error.value = `Video error: ${mediaErrorCodes[video.error.code] || 'UNKNOWN'} - ${video.error.message || 'no details'}`;
        } else {
          error.value = 'Video playback error';
        }
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
      isPlayPending,
      isMuted,
      error,
      isRecovering,
      recoveryAttempts,
      maxRecoveryAttempts,
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
.recovery-overlay,
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

.recovery-overlay {
  background: rgba(15, 23, 42, 0.95);
}

.recovery-text {
  color: #f59e0b;
  font-size: 1rem;
  margin: 0 0 8px 0;
  font-weight: 600;
}

.recovery-subtext {
  color: #94a3b8;
  font-size: 0.85rem;
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