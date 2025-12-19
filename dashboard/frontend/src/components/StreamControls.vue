<template>
  <div class="stream-controls-card">
    <h2>Stream Controls</h2>
    
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
                :checked="isKickToggleOn"
                @click="handleKickToggle"
                :disabled="isKickToggleDisabled"
              />
              <label for="kick-toggle" class="toggle-switch">
                <span class="toggle-text-off">OFF</span>
                <span class="toggle-text-on">ON</span>
              </label>
            </div>
            <div class="toggle-status-text" :class="{ active: isKickLive, pending: kickStartPending }">
              <span v-if="kickStartPending" class="kick-loading-spinner"></span>
              {{ kickStartPending ? 'Starting...' : (isKickLive ? 'Live on Kick' : 'Not Streaming') }}
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
      
      <div class="fallback-section">
        <h3 class="section-title">Fallback Source</h3>
        <div class="fallback-content">
          <div class="fallback-left">
          <div class="fallback-select-group">
            <v-select
              v-model="fallbackSource"
              :options="fallbackOptions"
              :reduce="option => option.value"
              :disabled="updatingFallback"
              :clearable="false"
              :searchable="false"
              @option:selected="handleFallbackSourceChange"
              class="fallback-select-vue"
              placeholder="Select fallback source"
            />
          </div>
          
          <div v-if="fallbackSource === 'IMAGE'" class="file-upload-group image-layout">
            <div class="upload-controls-column">
              <label class="fallback-label">Upload Image (PNG, JPG, GIF - Max 100MB)</label>
              <input
                type="file"
                id="image-upload-input"
                @change="handleImageUpload"
                accept=".png,.jpg,.jpeg,.gif"
                :disabled="uploadingFile"
                class="file-input-hidden"
              />
              <label
                for="image-upload-input"
                class="custom-file-button"
                :class="{ disabled: uploadingFile }"
              >
                📁 Choose Image File
              </label>
              <div v-if="selectedImageFile && !uploadSuccess" class="selected-file">
                Selected: {{ selectedImageFile }}
              </div>
              <div v-if="uploadingFile || isProcessing" class="upload-progress-container">
                <div class="progress-bar-wrapper">
                  <div
                    class="progress-bar"
                    :class="{ uploading: uploadingFile && !isProcessing, processing: isProcessing }"
                    :style="{ width: progressPercentage + '%' }"
                  ></div>
                </div>
                <div class="progress-status" :class="{ uploading: uploadingFile && !isProcessing, processing: isProcessing }">
                  <span v-if="uploadingFile && !isProcessing" class="upload-icon">
                    <svg class="upload-arrow" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                      <path d="M12 19V5M5 12l7-7 7 7"/>
                    </svg>
                  </span>
                  <span v-if="isProcessing" class="processing-icon">
                    <svg class="processing-gear" viewBox="0 0 24 24" fill="currentColor">
                      <path d="M12 15a3 3 0 100-6 3 3 0 000 6z"/>
                      <path fill-rule="evenodd" d="M19.4 15a1.65 1.65 0 00.33 1.82l.06.06a2 2 0 010 2.83 2 2 0 01-2.83 0l-.06-.06a1.65 1.65 0 00-1.82-.33 1.65 1.65 0 00-1 1.51V21a2 2 0 01-2 2 2 2 0 01-2-2v-.09A1.65 1.65 0 009 19.4a1.65 1.65 0 00-1.82.33l-.06.06a2 2 0 01-2.83 0 2 2 0 010-2.83l.06-.06a1.65 1.65 0 00.33-1.82 1.65 1.65 0 00-1.51-1H3a2 2 0 01-2-2 2 2 0 012-2h.09A1.65 1.65 0 004.6 9a1.65 1.65 0 00-.33-1.82l-.06-.06a2 2 0 010-2.83 2 2 0 012.83 0l.06.06a1.65 1.65 0 001.82.33H9a1.65 1.65 0 001-1.51V3a2 2 0 012-2 2 2 0 012 2v.09a1.65 1.65 0 001 1.51 1.65 1.65 0 001.82-.33l.06-.06a2 2 0 012.83 0 2 2 0 010 2.83l-.06.06a1.65 1.65 0 00-.33 1.82V9a1.65 1.65 0 001.51 1H21a2 2 0 012 2 2 2 0 01-2 2h-.09a1.65 1.65 0 00-1.51 1z"/>
                    </svg>
                  </span>
                  <span>{{ progressStatusText }}</span>
                </div>
              </div>
              <div v-if="uploadSuccess" class="upload-status success">✓ Upload successful</div>
              <div v-if="uploadError" class="upload-status error">{{ uploadError }}</div>
            </div>
          </div>
          
          <div v-if="fallbackSource === 'VIDEO'" class="file-upload-group">
            <label class="fallback-label">Upload Video (MP4, MOV, MPEG - Max 500MB)</label>
            <input
              type="file"
              id="video-upload-input"
              @change="handleVideoUpload"
              accept=".mp4,.mov,.mpeg"
              :disabled="uploadingFile"
              class="file-input-hidden"
            />
            <label
              for="video-upload-input"
              class="custom-file-button"
              :class="{ disabled: uploadingFile }"
            >
              📁 Choose Video File
            </label>
            <div v-if="selectedVideoFile && !uploadSuccess" class="selected-file">
              Selected: {{ selectedVideoFile }}
            </div>
            <div v-if="uploadingFile || isProcessing" class="upload-progress-container">
              <div class="progress-bar-wrapper">
                <div
                  class="progress-bar"
                  :class="{ uploading: uploadingFile && !isProcessing, processing: isProcessing }"
                  :style="{ width: progressPercentage + '%' }"
                ></div>
              </div>
              <div class="progress-status" :class="{ uploading: uploadingFile && !isProcessing, processing: isProcessing }">
                <span v-if="uploadingFile && !isProcessing" class="upload-icon">
                  <svg class="upload-arrow" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                    <path d="M12 19V5M5 12l7-7 7 7"/>
                  </svg>
                </span>
                <span v-if="isProcessing" class="processing-icon">
                  <svg class="processing-gear" viewBox="0 0 24 24" fill="currentColor">
                    <path d="M12 15a3 3 0 100-6 3 3 0 000 6z"/>
                    <path fill-rule="evenodd" d="M19.4 15a1.65 1.65 0 00.33 1.82l.06.06a2 2 0 010 2.83 2 2 0 01-2.83 0l-.06-.06a1.65 1.65 0 00-1.82-.33 1.65 1.65 0 00-1 1.51V21a2 2 0 01-2 2 2 2 0 01-2-2v-.09A1.65 1.65 0 009 19.4a1.65 1.65 0 00-1.82.33l-.06.06a2 2 0 01-2.83 0 2 2 0 010-2.83l.06-.06a1.65 1.65 0 00.33-1.82 1.65 1.65 0 00-1.51-1H3a2 2 0 01-2-2 2 2 0 012-2h.09A1.65 1.65 0 004.6 9a1.65 1.65 0 00-.33-1.82l-.06-.06a2 2 0 010-2.83 2 2 0 012.83 0l.06.06a1.65 1.65 0 001.82.33H9a1.65 1.65 0 001-1.51V3a2 2 0 012-2 2 2 0 012 2v.09a1.65 1.65 0 001 1.51 1.65 1.65 0 001.82-.33l.06-.06a2 2 0 012.83 0 2 2 0 010 2.83l-.06.06a1.65 1.65 0 00-.33 1.82V9a1.65 1.65 0 001.51 1H21a2 2 0 012 2 2 2 0 01-2 2h-.09a1.65 1.65 0 00-1.51 1z"/>
                  </svg>
                </span>
                <span>{{ progressStatusText }}</span>
              </div>
            </div>
            <div v-if="uploadSuccess" class="upload-status success">✓ Upload successful</div>
            <div v-if="uploadError" class="upload-status error">{{ uploadError }}</div>
          </div>
          
          <div v-if="fallbackSource === 'BROWSER'" class="url-input-group">
            <label class="fallback-label">Browser URL</label>
            <div class="url-input-container">
              <input
                v-model="browserUrl"
                type="url"
                placeholder="https://example.com"
                :disabled="updatingFallback"
                class="url-input"
                @keyup.enter="handleBrowserUrlUpdate"
              />
              <button
                @click="handleBrowserUrlUpdate"
                :disabled="updatingFallback || !browserUrl"
                class="url-update-btn"
              >
                Update
              </button>
            </div>
          </div>
          </div>
          <div v-if="fallbackSource !== 'BLACK'" class="fallback-right">
            <!-- Image Preview -->
            <div v-if="fallbackSource === 'IMAGE'" class="image-preview-container">
              <img v-if="imagePreviewUrl" :src="imagePreviewUrl" alt="Image preview" class="image-preview" />
              <div v-else class="image-placeholder">
                <span class="placeholder-icon">🖼️</span>
                <span class="placeholder-text">No image uploaded</span>
              </div>
            </div>
            
            <!-- Video Thumbnail Preview -->
            <div v-if="fallbackSource === 'VIDEO'" class="image-preview-container">
              <img v-if="videoThumbnailUrl" :src="videoThumbnailUrl" alt="Video thumbnail" class="image-preview" />
              <div v-else class="image-placeholder">
                <span class="placeholder-icon">🎬</span>
                <span class="placeholder-text">No video uploaded</span>
              </div>
            </div>
            
            <!-- Browser Preview -->
            <div v-if="fallbackSource === 'BROWSER'" class="browser-preview-container">
              <iframe
                v-if="browserUrl"
                :src="browserUrl"
                class="browser-preview-iframe"
                sandbox="allow-same-origin allow-scripts"
                @error="handleIframeError"
              ></iframe>
              <div v-else class="image-placeholder">
                <span class="placeholder-icon">🌐</span>
                <span class="placeholder-text">Enter URL above</span>
              </div>
              <div v-if="iframeError" class="iframe-error-overlay">
                <span class="placeholder-icon">⚠️</span>
                <span class="placeholder-text">Cannot preview: {{ browserUrl }}</span>
                <span class="placeholder-subtext">Cross-origin restrictions may apply</span>
              </div>
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
import vSelect from 'vue-select';
import 'vue-select/dist/vue-select.css';

export default {
  name: 'StreamControls',
  components: {
    ConfirmationModal,
    vSelect
  },
  props: {
    switcherHealth: {
      type: Object,
      default: () => ({})
    },
    fallbackConfig: {
      type: Object,
      default: () => ({
        source: 'BLACK',
        browserUrl: '',
        activeContainer: null
      })
    },
    uploadProgress: {
      type: Object,
      default: null
    }
  },
  data() {
    return {
      privacyEnabled: false,
      settingPrivacy: false,
      kickActionPending: false,
      kickStartPending: false,
      kickStartTimeoutId: null,
      showKickConfirmModal: false,
      pendingKickAction: null,
      kickChannelUrl: null,
      fallbackSource: 'BLACK',
      browserUrl: '',
      updatingFallback: false,
      uploadingFile: false,
      isProcessing: false,
      uploadPercent: 0,
      processingProgress: 0,
      processingMessage: '',
      uploadSuccess: false,
      uploadError: null,
      currentUploadId: null,
      selectedImageFile: null,
      selectedVideoFile: null,
      imagePreviewUrl: null,
      videoThumbnailUrl: null,
      iframeError: false,
      fallbackOptions: [
        { label: 'Black Screen', value: 'BLACK' },
        { label: 'Static Image', value: 'IMAGE' },
        { label: 'Video Loop', value: 'VIDEO' },
      ]
    };
  },
  computed: {
    progressPercentage() {
      if (this.isProcessing) {
        return this.processingProgress;
      }
      return this.uploadPercent;
    },
    progressStatusText() {
      if (this.isProcessing) {
        return this.processingMessage || `Processing... ${this.processingProgress}%`;
      }
      return `Uploading... ${this.uploadPercent}%`;
    },
    isKickLive() {
      return this.switcherHealth?.kick_streaming_enabled || false;
    },
    isKickToggleOn() {
      return this.isKickLive || this.kickStartPending;
    },
    isKickToggleDisabled() {
      return this.kickActionPending || this.kickStartPending;
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
    this.fetchFallbackConfig();
  },
  watch: {
    // Watch for kick streaming status changes from WebSocket updates
    'switcherHealth.kick_streaming_enabled': {
      handler(newKickEnabled) {
        if (newKickEnabled === true && this.kickStartPending) {
          console.log('[StreamControls] Kick container started, clearing pending state');
          this.kickStartPending = false;
          if (this.kickStartTimeoutId) {
            clearTimeout(this.kickStartTimeoutId);
            this.kickStartTimeoutId = null;
          }
        }
      },
      immediate: false
    },
    // Watch for privacy mode changes from WebSocket updates
    'switcherHealth.privacy_enabled': {
      handler(newPrivacyEnabled) {
        if (newPrivacyEnabled !== undefined && newPrivacyEnabled !== this.privacyEnabled) {
          console.log(`[StreamControls] Privacy mode auto-updated from ${this.privacyEnabled} to ${newPrivacyEnabled}`);
          this.privacyEnabled = newPrivacyEnabled;
        }
      },
      immediate: false
    },
    // Watch for fallbackConfig prop changes from WebSocket updates
    'fallbackConfig.source': {
      handler(newSource) {
        if (newSource && newSource !== this.fallbackSource) {
          console.log(`[StreamControls] Fallback source auto-updated from ${this.fallbackSource} to ${newSource}`);
          this.fallbackSource = newSource;
          
          // Update preview URLs when source changes
          if (newSource === 'IMAGE') {
            this.imagePreviewUrl = `/api/fallback/image?t=${Date.now()}`;
            this.videoThumbnailUrl = null;
          } else if (newSource === 'VIDEO') {
            this.videoThumbnailUrl = `/api/fallback/video-thumbnail?t=${Date.now()}`;
            this.imagePreviewUrl = null;
          } else {
            this.imagePreviewUrl = null;
            this.videoThumbnailUrl = null;
          }
        }
      },
      immediate: false
    },
    'fallbackConfig.browserUrl': {
      handler(newUrl) {
        if (newUrl && newUrl !== this.browserUrl) {
          this.browserUrl = newUrl;
        }
      },
      immediate: false
    },
    // Watch for upload progress from WebSocket
    uploadProgress: {
      handler(newProgress) {
        if (!newProgress) return;
        
        // Only handle progress for our current upload
        if (this.currentUploadId && newProgress.uploadId !== this.currentUploadId) {
          return;
        }
        
        console.log(`[StreamControls] WebSocket progress: ${newProgress.status} - ${newProgress.progress}%`);
        
        if (newProgress.status === 'processing') {
          this.isProcessing = true;
          this.processingProgress = newProgress.progress;
          this.processingMessage = newProgress.message;
        } else if (newProgress.status === 'completed') {
          this.isProcessing = false;
          this.processingProgress = 100;
          this.processingMessage = '';
          this.uploadSuccess = true;
          this.currentUploadId = null;
          
          // Load thumbnail for videos
          if (newProgress.fileType === 'video') {
            this.videoThumbnailUrl = `/api/fallback/video-thumbnail?t=${Date.now()}`;
          }
          
          setTimeout(() => {
            this.uploadSuccess = false;
          }, 3000);
        } else if (newProgress.status === 'error') {
          this.isProcessing = false;
          this.processingProgress = 0;
          this.processingMessage = '';
          this.uploadError = newProgress.error || 'Processing failed';
          this.currentUploadId = null;
        }
      },
      immediate: true,
      deep: true
    }
  },
  methods: {
    async fetchPrivacyMode() {
      try {
        const response = await fetch('/api/privacy');
        const data = await response.json();
        this.privacyEnabled = data.enabled || false;
      } catch (error) {
        console.error('[StreamControls] Error fetching privacy mode:', error);
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
        console.error('[StreamControls] Error fetching Kick channel:', error);
      }
    },
    async fetchFallbackConfig() {
      try {
        const response = await fetch('/api/fallback/config');
        const config = await response.json();
        this.fallbackSource = config.source || 'BLACK';
        this.browserUrl = config.browserUrl || '';
        
        // Load previews based on source type
        if (config.source === 'IMAGE') {
          // Add cache buster to ensure fresh image is loaded
          this.imagePreviewUrl = `/api/fallback/image?t=${Date.now()}`;
          this.videoThumbnailUrl = null;
        } else if (config.source === 'VIDEO') {
          // Load video thumbnail
          this.videoThumbnailUrl = `/api/fallback/video-thumbnail?t=${Date.now()}`;
          this.imagePreviewUrl = null;
        } else {
          this.imagePreviewUrl = null;
          this.videoThumbnailUrl = null;
        }
        
        // Reset iframe error when config loads
        this.iframeError = false;
        
        console.log('[StreamControls] Fallback config loaded:', config);
      } catch (error) {
        console.error('[StreamControls] Error fetching fallback config:', error);
      }
    },
    async handleFallbackSourceChange(value) {
      const newSource = value?.value || value;
      this.fallbackSource = newSource;
      if (this.updatingFallback) return;
      
      this.updatingFallback = true;
      this.uploadSuccess = false;
      this.uploadError = null;
      this.iframeError = false;
      
      // Load appropriate preview based on source type
      if (newSource === 'IMAGE') {
        this.imagePreviewUrl = `/api/fallback/image?t=${Date.now()}`;
        this.videoThumbnailUrl = null;
      } else if (newSource === 'VIDEO') {
        this.videoThumbnailUrl = `/api/fallback/video-thumbnail?t=${Date.now()}`;
        this.imagePreviewUrl = null;
      } else {
        this.imagePreviewUrl = null;
        this.videoThumbnailUrl = null;
      }
      
      try {
        const response = await fetch('/api/fallback/config', {
          method: 'POST',
          headers: {
            'Content-Type': 'application/json'
          },
          body: JSON.stringify({
            source: this.fallbackSource,
            browserUrl: this.browserUrl
          })
        });
        
        if (!response.ok) {
          const errorData = await response.json();
          throw new Error(errorData.error || 'Failed to update fallback source');
        }
        
        const result = await response.json();
        console.log('[StreamControls] Fallback source updated:', result);
      } catch (error) {
        console.error('[StreamControls] Error updating fallback source:', error);
        alert(`Failed to update fallback source: ${error.message}`);
      } finally {
        this.updatingFallback = false;
      }
    },
    async handleImageUpload(event) {
      const file = event.target.files[0];
      if (!file) {
        this.selectedImageFile = null;
        this.imagePreviewUrl = null;
        return;
      }
      
      this.selectedImageFile = file.name;
      this.uploadingFile = true;
      this.isProcessing = false;
      this.uploadPercent = 0;
      this.processingProgress = 0;
      this.uploadSuccess = false;
      this.uploadError = null;
      
      // Create preview immediately using FileReader
      const reader = new FileReader();
      reader.onload = (e) => {
        this.imagePreviewUrl = e.target.result;
      };
      reader.readAsDataURL(file);
      
      const formData = new FormData();
      formData.append('image', file);
      
      // Use XMLHttpRequest to track upload progress
      const xhr = new XMLHttpRequest();
      
      xhr.upload.addEventListener('progress', (e) => {
        if (e.lengthComputable) {
          const percentComplete = Math.round((e.loaded / e.total) * 100);
          this.uploadPercent = percentComplete;
          console.log(`[StreamControls] Image upload progress: ${percentComplete}%`);
        }
      });
      
      xhr.addEventListener('load', () => {
        if (xhr.status >= 200 && xhr.status < 300) {
          try {
            const result = JSON.parse(xhr.responseText);
            console.log('[StreamControls] Image uploaded, processing started:', result);
            
            // Store upload ID to match with WebSocket progress
            this.currentUploadId = result.uploadId;
            
            // Upload complete, now waiting for processing via WebSocket
            this.uploadingFile = false;
            this.uploadPercent = 100;
            this.isProcessing = true;
            this.processingProgress = 0;
            this.processingMessage = 'Starting processing...';
            
          } catch (error) {
            this.uploadError = 'Invalid response from server';
            this.uploadingFile = false;
            this.isProcessing = false;
          }
        } else {
          try {
            const errorData = JSON.parse(xhr.responseText);
            this.uploadError = errorData.error || 'Upload failed';
          } catch {
            this.uploadError = `Upload failed with status ${xhr.status}`;
          }
          this.imagePreviewUrl = null;
          this.uploadingFile = false;
          this.isProcessing = false;
        }
        event.target.value = '';
        this.selectedImageFile = null;
      });
      
      xhr.addEventListener('error', () => {
        console.error('[StreamControls] Error uploading image');
        this.uploadError = 'Network error during upload';
        this.imagePreviewUrl = null;
        this.uploadingFile = false;
        this.isProcessing = false;
        this.uploadPercent = 0;
        event.target.value = '';
        this.selectedImageFile = null;
      });
      
      xhr.open('POST', '/api/fallback/upload-image');
      xhr.send(formData);
    },
    async handleVideoUpload(event) {
      const file = event.target.files[0];
      if (!file) {
        this.selectedVideoFile = null;
        this.videoThumbnailUrl = null;
        return;
      }
      
      this.selectedVideoFile = file.name;
      this.uploadingFile = true;
      this.isProcessing = false;
      this.uploadPercent = 0;
      this.processingProgress = 0;
      this.uploadSuccess = false;
      this.uploadError = null;
      
      const formData = new FormData();
      formData.append('video', file);
      
      // Use XMLHttpRequest to track upload progress
      const xhr = new XMLHttpRequest();
      
      xhr.upload.addEventListener('progress', (e) => {
        if (e.lengthComputable) {
          const percentComplete = Math.round((e.loaded / e.total) * 100);
          this.uploadPercent = percentComplete;
          console.log(`[StreamControls] Video upload progress: ${percentComplete}%`);
        }
      });
      
      xhr.addEventListener('load', () => {
        if (xhr.status >= 200 && xhr.status < 300) {
          try {
            const result = JSON.parse(xhr.responseText);
            console.log('[StreamControls] Video uploaded, processing started:', result);
            
            // Store upload ID to match with WebSocket progress
            this.currentUploadId = result.uploadId;
            
            // Upload complete, now waiting for processing via WebSocket
            this.uploadingFile = false;
            this.uploadPercent = 100;
            this.isProcessing = true;
            this.processingProgress = 0;
            this.processingMessage = 'Starting processing...';
            
          } catch (error) {
            this.uploadError = 'Invalid response from server';
            this.uploadingFile = false;
            this.isProcessing = false;
          }
        } else {
          try {
            const errorData = JSON.parse(xhr.responseText);
            this.uploadError = errorData.error || 'Upload failed';
          } catch {
            this.uploadError = `Upload failed with status ${xhr.status}`;
          }
          this.videoThumbnailUrl = null;
          this.uploadingFile = false;
          this.isProcessing = false;
        }
        event.target.value = '';
        this.selectedVideoFile = null;
      });
      
      xhr.addEventListener('error', () => {
        console.error('[StreamControls] Error uploading video');
        this.uploadError = 'Network error during upload';
        this.videoThumbnailUrl = null;
        this.uploadingFile = false;
        this.isProcessing = false;
        this.uploadPercent = 0;
        event.target.value = '';
        this.selectedVideoFile = null;
      });
      
      xhr.open('POST', '/api/fallback/upload-video');
      xhr.send(formData);
    },
    handleIframeError() {
      console.warn('[StreamControls] Iframe failed to load, likely due to cross-origin restrictions');
      this.iframeError = true;
    },
    async handleBrowserUrlUpdate() {
      if (!this.browserUrl || this.updatingFallback) return;
      
      this.updatingFallback = true;
      this.iframeError = false;
      
      try {
        const response = await fetch('/api/fallback/config', {
          method: 'POST',
          headers: {
            'Content-Type': 'application/json'
          },
          body: JSON.stringify({
            source: 'BROWSER',
            browserUrl: this.browserUrl
          })
        });
        
        if (!response.ok) {
          const errorData = await response.json();
          throw new Error(errorData.error || 'Failed to update browser URL');
        }
        
        const result = await response.json();
        console.log('[StreamControls] Browser URL updated:', result);
      } catch (error) {
        console.error('[StreamControls] Error updating browser URL:', error);
        alert(`Failed to update browser URL: ${error.message}`);
      } finally {
        this.updatingFallback = false;
      }
    },
    handlePrivacyToggle(event) {
      const enabled = event.target.checked;
      this.setPrivacyMode(enabled);
    },
    async setPrivacyMode(enabled) {
      if (this.settingPrivacy) return;
      
      this.settingPrivacy = true;
      console.log(`[StreamControls] Setting privacy mode to: ${enabled}`);
      
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
        console.log(`[StreamControls] Privacy mode set successfully:`, result);
        this.privacyEnabled = enabled;
      } catch (error) {
        console.error(`[StreamControls] Error setting privacy mode:`, error);
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
      const isStarting = this.pendingKickAction === 'start';
      
      try {
        const endpoint = isStarting
          ? '/api/kick/start'
          : '/api/kick/stop';
        
        console.log(`[StreamControls] ${isStarting ? 'Starting' : 'Stopping'} Kick stream...`);
        
        const response = await fetch(endpoint, {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' }
        });
        
        if (!response.ok) {
          const data = await response.json();
          throw new Error(data.error || `Failed to ${this.pendingKickAction} stream`);
        }
        
        console.log(`[StreamControls] Kick stream ${this.pendingKickAction} successful`);
        
        // If starting, set pending state and wait for container state update
        if (isStarting) {
          this.kickStartPending = true;
          console.log('[StreamControls] Waiting for container to start...');
          
          // Clear any existing timeout
          if (this.kickStartTimeoutId) {
            clearTimeout(this.kickStartTimeoutId);
          }
          
          // Set 5-second timeout to clear pending state if no update received
          this.kickStartTimeoutId = setTimeout(() => {
            if (this.kickStartPending) {
              console.log('[StreamControls] Timeout waiting for container state update, clearing pending state');
              this.kickStartPending = false;
              this.kickStartTimeoutId = null;
            }
          }, 5000);
        }
      } catch (error) {
        console.error('[StreamControls] Kick toggle error:', error);
        alert(`Failed to ${this.pendingKickAction} Kick stream: ${error.message}`);
      } finally {
        this.kickActionPending = false;
        this.showKickConfirmModal = false;
        this.pendingKickAction = null;
      }
    },
    beforeUnmount() {
      // Clean up timeout on component unmount
      if (this.kickStartTimeoutId) {
        clearTimeout(this.kickStartTimeoutId);
        this.kickStartTimeoutId = null;
      }
    }
  }
};
</script>

<style scoped>
.stream-controls-card {
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

.controls-grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(250px, 1fr));
  gap: 20px;
}

.control-section,
.scene-selection-section {
  background: #0f172a;
  border-radius: 6px;
  padding: 15px;
}

.fallback-section {
  background: #0f172a;
  border-radius: 6px;
  padding: 15px;
  grid-column: span 2;
}

@media (max-width: 768px) {
  .fallback-section {
    grid-column: span 1;
  }
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

.toggle-status-text.pending {
  color: #f59e0b;
  font-weight: 600;
  display: flex;
  align-items: center;
  gap: 6px;
}

.kick-loading-spinner {
  display: inline-block;
  width: 14px;
  height: 14px;
  border: 2px solid #f59e0b;
  border-top-color: transparent;
  border-radius: 50%;
  animation: kick-spinner-spin 0.8s linear infinite;
}

@keyframes kick-spinner-spin {
  from {
    transform: rotate(0deg);
  }
  to {
    transform: rotate(360deg);
  }
}

.fallback-content {
  display: flex;
  flex-direction: row;
  gap: 15px;
}

.fallback-left {
  flex-grow: 1;
}

.fallback-select-group {
  display: flex;
  flex-direction: column;
  gap: 8px;
}

.fallback-label {
  font-size: 0.875rem;
  color: #e2e8f0;
  font-weight: 500;
}

.fallback-select-vue {
  width: 100%;
  position: relative;
}

/* Vue-select custom dark theme styling */
.fallback-select-vue :deep(.vs__dropdown-toggle) {
  background: #1e293b;
  border: 1px solid #334155;
  border-radius: 6px;
  padding: 6px 8px;
  transition: border-color 0.2s ease;
}

.fallback-select-vue :deep(.vs__dropdown-toggle:hover) {
  border-color: #3b82f6;
}

.fallback-select-vue :deep(.vs__selected) {
  color: #e2e8f0;
  font-size: 0.9rem;
  margin: 2px;
  padding: 2px 6px;
}

.fallback-select-vue :deep(.vs__search),
.fallback-select-vue :deep(.vs__search:focus) {
  color: #e2e8f0;
  margin: 2px 0;
  padding: 2px;
}

.fallback-select-vue :deep(.vs__search::placeholder) {
  color: #64748b;
}

.fallback-select-vue :deep(.vs__actions) {
  padding: 2px 6px;
}

.fallback-select-vue :deep(.vs__open-indicator) {
  fill: #94a3b8;
  transition: transform 0.2s ease;
}

.fallback-select-vue :deep(.vs__dropdown-toggle:hover .vs__open-indicator) {
  fill: #3b82f6;
}

.fallback-select-vue :deep(.vs__clear) {
  fill: #94a3b8;
  transition: fill 0.2s ease;
}

.fallback-select-vue :deep(.vs__clear:hover) {
  fill: #ef4444;
}

.fallback-select-vue :deep(.vs__dropdown-menu) {
  background: #1e293b;
  border: 1px solid #334155;
  border-radius: 6px;
  margin-top: 4px;
  padding: 4px 0;
  box-shadow: 0 4px 6px rgba(0, 0, 0, 0.3);
}

.fallback-select-vue :deep(.vs__dropdown-option) {
  color: #e2e8f0;
  padding: 8px 12px;
  transition: background-color 0.15s ease, color 0.15s ease;
}

.fallback-select-vue :deep(.vs__dropdown-option--highlight) {
  background: #3b82f6;
  color: #ffffff;
}

.fallback-select-vue :deep(.vs__dropdown-option--selected) {
  background: rgba(59, 130, 246, 0.2);
  color: #ffffff;
  font-weight: 600;
}

.fallback-select-vue :deep(.vs__dropdown-option--disabled) {
  color: #64748b;
  cursor: not-allowed;
}

.fallback-select-vue :deep(.vs__no-options) {
  color: #94a3b8;
  padding: 12px;
  text-align: center;
}

.fallback-select-vue :deep(.vs__spinner) {
  border-left-color: #3b82f6;
}

/* Disabled state */
.fallback-select-vue :deep(.vs--disabled .vs__dropdown-toggle) {
  background: #0f172a;
  opacity: 0.6;
  cursor: not-allowed;
}

.fallback-select-vue :deep(.vs--disabled .vs__selected) {
  color: #64748b;
}

.fallback-select-vue :deep(.vs--disabled .vs__open-indicator) {
  fill: #64748b;
}

.file-upload-group {
  display: flex;
  flex-direction: column;
  gap: 12px;
}

.file-upload-group.image-layout {
  flex-direction: row;
  gap: 20px;
  min-height: 200px;
}

.upload-controls-column {
  display: flex;
  flex-direction: column;
  gap: 12px;
  flex: 1;
  min-width: 0;
}

.upload-content-wrapper {
  display: flex;
  gap: 16px;
  align-items: flex-start;
}

.upload-controls {
  display: flex;
  flex-direction: column;
  gap: 8px;
  flex: 1;
  min-width: 0;
}

.image-preview-container {
  flex-shrink: 0;
  width: auto;
  height: 200px;
  max-height: 200px;
  border: 2px solid #334155;
  border-radius: 8px;
  overflow: hidden;
  background: #0f172a;
  display: flex;
  align-items: center;
  justify-content: center;
}

.image-preview {
  max-width: 100%;
  max-height: 100%;
  height: auto;
  width: auto;
  object-fit: contain;
}

.image-placeholder {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: 12px;
  color: #64748b;
  padding: 20px;
  text-align: center;
}

.placeholder-icon {
  font-size: 3rem;
  opacity: 0.5;
}

.placeholder-text {
  font-size: 0.875rem;
  font-weight: 500;
}

@media (max-width: 768px) {
  .file-upload-group.image-layout {
    flex-direction: column;
    min-height: auto;
  }
  
  .image-preview-container {
    width: 100%;
    height: 200px;
    max-height: 200px;
  }
}

.file-input-hidden {
  opacity: 0;
  position: absolute;
  width: 0;
  height: 0;
  pointer-events: none;
}

.custom-file-button {
  display: inline-block;
  padding: 10px 20px;
  background: #3b82f6;
  color: white;
  border: none;
  border-radius: 6px;
  font-size: 0.875rem;
  font-weight: 600;
  cursor: pointer;
  transition: all 0.2s ease;
  text-align: center;
  user-select: none;
}

.custom-file-button:hover {
  background: #2563eb;
  transform: translateY(-1px);
  box-shadow: 0 4px 8px rgba(59, 130, 246, 0.3);
}

.custom-file-button:active {
  background: #1d4ed8;
  transform: translateY(0);
  box-shadow: 0 2px 4px rgba(59, 130, 246, 0.3);
}

.custom-file-button.disabled {
  background: #475569;
  cursor: not-allowed;
  opacity: 0.6;
  pointer-events: none;
  transform: none;
}

.selected-file {
  font-size: 0.875rem;
  color: #94a3b8;
  padding: 8px 0;
  font-style: italic;
}

.upload-status {
  font-size: 0.875rem;
  padding: 8px 12px;
  border-radius: 4px;
  text-align: center;
}

.upload-status.success {
  background: rgba(16, 185, 129, 0.1);
  color: #10b981;
  border: 1px solid #10b981;
}

.upload-status.error {
  background: rgba(239, 68, 68, 0.1);
  color: #ef4444;
  border: 1px solid #ef4444;
}

/* Upload progress container with progress bar */
.upload-progress-container {
  display: flex;
  flex-direction: column;
  gap: 8px;
  padding: 12px;
  background: rgba(30, 41, 59, 0.8);
  border-radius: 8px;
  border: 1px solid #334155;
}

.progress-bar-wrapper {
  width: 100%;
  height: 8px;
  background: #1e293b;
  border-radius: 4px;
  overflow: hidden;
}

.progress-bar {
  height: 100%;
  border-radius: 4px;
  transition: width 0.3s ease-out;
}

.progress-bar.uploading {
  background: linear-gradient(90deg, #3b82f6, #60a5fa);
  animation: progress-pulse 1.5s ease-in-out infinite;
}

.progress-bar.processing {
  background: linear-gradient(90deg, #f97316, #fb923c);
  animation: progress-pulse 1.5s ease-in-out infinite;
}

@keyframes progress-pulse {
  0%, 100% {
    opacity: 1;
  }
  50% {
    opacity: 0.7;
  }
}

.progress-status {
  display: flex;
  align-items: center;
  gap: 8px;
  font-size: 0.875rem;
  font-weight: 500;
}

.progress-status.uploading {
  color: #3b82f6;
}

.progress-status.processing {
  color: #f97316;
}

.upload-icon,
.processing-icon {
  display: flex;
  align-items: center;
  justify-content: center;
}

.upload-arrow {
  width: 18px;
  height: 18px;
  animation: upload-bounce 0.8s ease-in-out infinite;
}

.processing-gear {
  width: 18px;
  height: 18px;
  animation: gear-spin 1.5s linear infinite;
}

@keyframes upload-bounce {
  0%, 100% {
    transform: translateY(0);
  }
  50% {
    transform: translateY(-4px);
  }
}

@keyframes gear-spin {
  from {
    transform: rotate(0deg);
  }
  to {
    transform: rotate(360deg);
  }
}

.url-input-group {
  display: flex;
  flex-direction: column;
  gap: 8px;
}

.url-input-container {
  display: flex;
  gap: 8px;
}

.url-input {
  flex: 1;
  padding: 10px 12px;
  background: #1e293b;
  border: 1px solid #334155;
  border-radius: 6px;
  color: #e2e8f0;
  font-size: 0.9rem;
  transition: border-color 0.2s ease;
}

.url-input:hover:not(:disabled) {
  border-color: #3b82f6;
}

.url-input:focus {
  outline: none;
  border-color: #3b82f6;
  box-shadow: 0 0 0 3px rgba(59, 130, 246, 0.1);
}

.url-input:disabled {
  opacity: 0.5;
  cursor: not-allowed;
}

.url-update-btn {
  padding: 10px 20px;
  background: #3b82f6;
  color: white;
  border: none;
  border-radius: 6px;
  font-size: 0.875rem;
  font-weight: 600;
  cursor: pointer;
  transition: all 0.2s ease;
  white-space: nowrap;
}

.url-update-btn:hover:not(:disabled) {
  background: #2563eb;
  transform: translateY(-1px);
  box-shadow: 0 4px 8px rgba(59, 130, 246, 0.3);
}

.url-update-btn:active:not(:disabled) {
  background: #1d4ed8;
  transform: translateY(0);
}

.url-update-btn:disabled {
  background: #475569;
  cursor: not-allowed;
  opacity: 0.6;
}

/* Browser Preview Styles */
.browser-preview-container {
  position: relative;
  flex-shrink: 0;
  width: auto;
  height: 200px;
  max-height: 200px;
  border: 2px solid #334155;
  border-radius: 8px;
  overflow: hidden;
  background: #0f172a;
  display: flex;
  align-items: center;
  justify-content: center;
}

.browser-preview-iframe {
  width: 100%;
  height: 100%;
  border: none;
  background: white;
}

.iframe-error-overlay {
  position: absolute;
  top: 0;
  left: 0;
  right: 0;
  bottom: 0;
  background: rgba(15, 23, 42, 0.95);
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: 12px;
  color: #f59e0b;
  padding: 20px;
  text-align: center;
  z-index: 10;
}

.placeholder-subtext {
  font-size: 0.75rem;
  color: #94a3b8;
  font-weight: 400;
}

@media (max-width: 768px) {
  .browser-preview-container {
    width: 100%;
    height: 200px;
    max-height: 200px;
  }
}
</style>