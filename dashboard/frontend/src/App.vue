<template>
  <div class="app">
    <header class="header">
      <span>
      <button
        @click="toggleInterface"
        class="interface-toggle"
        :title="interfaceMode === 'simplified' ? 'Switch to Full View' : 'Switch to Simplified View'"
      >
        <span class="toggle-icon">{{ interfaceMode === 'simplified' ? '📊' : '⚡' }}</span>
      </button>
      <h1>Latest Relayer</h1>
      </span>
      <div class="header-controls">
        <div class="connection-status">
          <span class="status-indicator" :class="{ connected: wsConnected, disconnected: !wsConnected }">
            {{ wsConnected ? '● Connected' : '○ Disconnected' }}
          </span>
          <span class="last-update">Last update: {{ lastUpdate }}</span>
        </div>
      </div>
    </header>

    <main class="main-content">
      <div v-if="error" class="error-banner">
        <strong>Error:</strong> {{ error }}
      </div>

      <!-- Backend Connection Error Warning -->
      <div v-if="data.containersFetchError" class="backend-error-banner">
        <div class="backend-error-content">
          <span class="backend-error-icon">🚨</span>
          <div class="backend-error-text">
            <strong>CRITICAL: Backend Controller Unavailable</strong>
            <p>Cannot communicate with controller service. Container status information may be unavailable or outdated.</p>
            <p class="error-detail">Error: {{ data.containersFetchError }}</p>
          </div>
        </div>
      </div>

      <!-- Simplified Interface -->
      <SimplifiedView
        v-if="interfaceMode === 'simplified'"
        :containers="data.containers"
        :currentScene="data.currentScene"
        :sceneDurationSeconds="data.sceneDurationSeconds"
        :rtmpStats="data.rtmpStats"
        :switcherHealth="data.switcherHealth"
        :streamStatus="data.streamStatus"
        :currentSource="data.currentSource"
      />

      <!-- Full Interface -->
      <template v-else>
        <div class="stream-info">
          <div v-if="statusSuccess.length > 0" class="status-success">
            <div
              v-for="(success, index) in statusSuccess"
              :key="index"
              class="success-item"
            >
              <span class="success-icon">✅</span>
              {{ success }}
            </div>
          </div>
          
          <div v-if="statusWarnings.length > 0" class="status-warnings">
            <div
              v-for="(warning, index) in statusWarnings"
              :key="index"
              class="warning-item"
            >
              <span class="warning-icon">⛔</span>
              {{ warning }}
            </div>
          </div>
        </div>

        <div class="dashboard-grid">
          <SystemMetrics
            :metrics="data.systemMetrics"
          />
          <StreamStats
            :stats="data.rtmpStats"
            :streamStatus="data.streamStatus"
            :currentScene="data.currentScene"
            :sceneDurationSeconds="data.sceneDurationSeconds"
            :cameraConfig="data.cameraConfig"
            :switcherHealth="data.switcherHealth"
            :fallbackConfig="data.fallbackConfig"
            :currentSource="data.currentSource"
          />
          <StreamControls
            :switcherHealth="data.switcherHealth"
            :fallbackConfig="data.fallbackConfig"
            :uploadProgress="uploadProgress"
          />
        </div>

        <KickSettings />

        <div class="containers-section">
          <ContainerGrid
            :containers="data.containers"
            @container-status-changed="updateContainerStatus"
          />
          <ContainerLogs
              :containers="data.containers"
              :logMessages="logMessages"
              :wsReconnectCount="wsReconnectCount"
            />
        </div>
      </template>
    </main>

    <footer class="footer">
      <a
        v-if="lastCommitTime"
        :href="lastCommitUrl"
        target="_blank"
        rel="noopener noreferrer"
        class="footer-link"
      >
        Latest Relayer last updated {{ formattedTimeAgo }}.
      </a>
      <span v-else class="footer-loading">Loading update info...</span>
    </footer>
  </div>
</template>

<script>
import { ref, computed, onMounted, onUnmounted, provide, watch } from 'vue';
import { WebSocketService } from './services/websocket.js';
import SystemMetrics from './components/SystemMetrics.vue';
import StreamStats from './components/StreamStats.vue';
import StreamControls from './components/StreamControls.vue';
import KickSettings from './components/KickSettings.vue';
import ContainerGrid from './components/ContainerGrid.vue';
import ContainerLogs from './components/ContainerLogs.vue';
import SimplifiedView from './components/SimplifiedView.vue';

export default {
  name: 'App',
  components: {
    SystemMetrics,
    StreamStats,
    StreamControls,
    KickSettings,
    ContainerGrid,
    ContainerLogs,
    SimplifiedView
  },
  setup() {
    // Interface mode with localStorage persistence
    const STORAGE_KEY = 'streamDashboardMode';
    const storedMode = localStorage.getItem(STORAGE_KEY);
    const interfaceMode = ref(storedMode || 'simplified'); // 'simplified' or 'full'
    
    const wsService = new WebSocketService();
    const wsConnected = ref(false);
    const lastUpdate = ref('Never');
    const error = ref(null);
    const pollingInterval = ref(2000);
    
    // GitHub commit tracking
    const lastCommitTime = ref(null);
    const lastCommitUrl = ref('https://github.com/nulligun/latestrelayer/commits');
    const currentTime = ref(Date.now());
    let commitRefreshInterval = null;
    let timeUpdateInterval = null;
    
    // Provide WebSocket service to child components
    provide('wsService', wsService);
    
    // Track pending container operations
    const pendingOperations = ref({});
    
    // Track scene switching state
    const switchingScene = ref(null);
    let switchingTimeout = null;
    let previousScene = null;

    // Computed properties for stream status
    const statusWarnings = computed(() => {
      const warnings = [];
      
      if (!data.value.streamStatus?.kickStreamingEnabled) {
        warnings.push('NOT LIVE ON KICK!');
      }
      
      // Check if ffmpeg-srt-live container is running and healthy (indicates SRT camera connection on port 1937)
      const srtLiveContainer = data.value.containers?.find(c => c.name === 'ffmpeg-srt-live');
      if (!srtLiveContainer?.running || srtLiveContainer?.health !== 'healthy') {
        warnings.push('CAMERA NOT CONNECTED');
      }
      
      // Check for stream incompatibility (camera codec doesn't match fallback settings)
      if (data.value.switcherHealth?.stream_incompatible) {
        warnings.push('CAMERA STREAM INCOMPATIBLE - Settings may not match expected format');
      }
      
      return warnings;
    });

    const statusSuccess = computed(() => {
      const successes = [];
      
      if (data.value.streamStatus?.kickStreamingEnabled) {
        successes.push("WE'RE LIVE ON KICK!");
      }
      
      return successes;
    });
    
    // Store for log messages to pass to ContainerLogs
    const logMessages = ref([]);
    
    // Reconnection counter to notify ContainerLogs to resubscribe
    const wsReconnectCount = ref(0);
    
    // Upload progress tracking for StreamControls
    const uploadProgress = ref(null);
    
    const data = ref({
      timestamp: null,
      containers: [],
      containersFetchError: null,
      systemMetrics: {
        cpu: 0,
        memory: 0,
        load: [0],
        memoryUsed: 0,
        memoryTotal: 0
      },
      rtmpStats: {
        inboundBandwidth: 0,
        streams: {}
      },
      currentScene: null,
      sourceAvailability: null,
      streamStatus: {
        isOnline: false,
        durationSeconds: 0
      },
      sceneDurationSeconds: 0,
      cameraConfig: null,
      switcherHealth: {
        status: 'unavailable',
        current_scene: 'unknown',
        srt_connected: false,
        privacy_enabled: false,
        kick_streaming_enabled: false,
        stream_incompatible: false
      },
      fallbackConfig: {
        source: 'BLACK',
        imagePath: '/app/shared/offline.png',
        videoPath: '/app/shared/offline.mp4',
        browserUrl: 'https://example.com'
      },
      currentSource: 'camera'
    });

    const formatTime = (timestamp) => {
      if (!timestamp) return 'Never';
      const date = new Date(timestamp);
      return date.toLocaleTimeString();
    };

    // Format time ago with exactly two units, minutes as smallest
    const formatTimeAgo = (dateString) => {
      if (!dateString) return '';
      
      const date = new Date(dateString);
      const now = currentTime.value;
      const diffMs = now - date.getTime();
      
      // Convert to minutes (smallest unit)
      const totalMinutes = Math.floor(diffMs / (1000 * 60));
      
      if (totalMinutes < 1) {
        return '1 minute'; // Show at least 1 minute
      }
      
      // Define time units in descending order
      const units = [
        { name: 'year', minutes: 365 * 24 * 60 },
        { name: 'month', minutes: 30 * 24 * 60 },
        { name: 'week', minutes: 7 * 24 * 60 },
        { name: 'day', minutes: 24 * 60 },
        { name: 'hour', minutes: 60 },
        { name: 'minute', minutes: 1 }
      ];
      
      let remaining = totalMinutes;
      const parts = [];
      
      for (const unit of units) {
        if (remaining >= unit.minutes) {
          const count = Math.floor(remaining / unit.minutes);
          remaining = remaining % unit.minutes;
          parts.push(`${count} ${unit.name}${count !== 1 ? 's' : ''}`);
          
          // Stop after two parts
          if (parts.length === 2) break;
        }
      }
      
      // If we only have one part (e.g., "5 minutes"), add "0" of the next smaller unit
      // But only if there's a meaningful smaller unit to show
      if (parts.length === 1) {
        // Find the current unit index
        const currentUnitName = parts[0].split(' ')[1].replace(/s$/, '');
        const currentIndex = units.findIndex(u => u.name === currentUnitName);
        
        // If there's a smaller unit and it makes sense to show it
        if (currentIndex < units.length - 1 && remaining > 0) {
          const nextUnit = units[currentIndex + 1];
          const nextCount = Math.floor(remaining / nextUnit.minutes);
          if (nextCount > 0) {
            parts.push(`${nextCount} ${nextUnit.name}${nextCount !== 1 ? 's' : ''}`);
          }
        }
      }
      
      return parts.join(' ') + ' ago';
    };
    
    // Computed property for formatted time
    const formattedTimeAgo = computed(() => {
      return formatTimeAgo(lastCommitTime.value);
    });
    
    // Fetch last commit from GitHub
    const fetchLastCommit = async () => {
      try {
        const response = await fetch('/api/github/last-commit');
        if (response.ok) {
          const data = await response.json();
          lastCommitTime.value = data.date;
          if (data.url) {
            lastCommitUrl.value = data.url;
          }
          console.log('[app] Last commit:', data.date);
        }
      } catch (err) {
        console.error('[app] Error fetching last commit:', err.message);
      }
    };

    const handleMessage = (message) => {
      // Handle different message types from WebSocket
      if (message.type === 'container_update') {
        handleContainerUpdate(message);
      } else if (message.type === 'metrics_update') {
        // Handle system metrics updates
        if (message.systemMetrics) {
          data.value.systemMetrics = message.systemMetrics;
        }
      } else if (message.type === 'scene_change') {
        // Handle dedicated scene change messages
        console.log(`[app] Scene change received: ${message.currentScene}`);
        data.value.currentScene = message.currentScene;
        if (message.privacyEnabled !== undefined) {
          data.value.switcherHealth.privacy_enabled = message.privacyEnabled;
        }
        // Clear switching state if scene matches what we were switching to
        if (switchingScene.value && message.currentScene === switchingScene.value) {
          console.log('[app] Scene switch completed, clearing switching state');
          switchingScene.value = null;
          if (switchingTimeout) {
            clearTimeout(switchingTimeout);
            switchingTimeout = null;
          }
        }
      } else if (message.type === 'privacy_change') {
        // Handle dedicated privacy change messages
        console.log(`[app] Privacy change received: ${message.privacyEnabled}`);
        data.value.switcherHealth.privacy_enabled = message.privacyEnabled;
        if (message.currentScene !== undefined) {
          data.value.currentScene = message.currentScene;
        }
      } else if (message.type === 'input_source_change') {
        // Handle input source change messages
        console.log(`[app] Input source change received: ${message.currentSource}`);
        data.value.currentSource = message.currentSource;
      } else if (message.type === 'log_snapshot' || message.type === 'new_logs') {
        // Forward log messages to ContainerLogs component
        console.log(`[app] Forwarding ${message.type} message for ${message.container}`);
        logMessages.value = [...logMessages.value, message];
      } else if (message.type === 'upload_progress') {
        // Forward upload progress to StreamControls
        console.log(`[app] Upload progress: ${message.status} - ${message.progress}%`);
        uploadProgress.value = message;
        
        // Clear progress after completion or error
        if (message.status === 'completed' || message.status === 'error') {
          setTimeout(() => {
            uploadProgress.value = null;
          }, 3000);
        }
      }
      
      // Mark connection as active
      wsConnected.value = true;
      error.value = null;
    };

    const handleContainerUpdate = (message) => {
      const now = Date.now();
      const OPERATION_TIMEOUT = 30000; // 30 seconds
      
      // Update last update time
      lastUpdate.value = formatTime(message.timestamp);
      
      // Update scene from status_change messages (fallback mechanism)
      if (message.currentScene !== undefined) {
        console.log(`[app] Scene from container_update: ${message.currentScene}`);
        data.value.currentScene = message.currentScene;
      }
      if (message.privacyEnabled !== undefined) {
        data.value.switcherHealth.privacy_enabled = message.privacyEnabled;
      }
      if (message.currentSource !== undefined) {
        console.log(`[app] Input source from container_update: ${message.currentSource}`);
        data.value.currentSource = message.currentSource;
      }
      
      // Process containers and apply pending operations
      const processedContainers = (message.containers || []).map(container => {
        const pending = pendingOperations.value[container.name];
        
        if (!pending) {
          return container;
        }
        
        // Check if operation timed out
        const elapsed = now - pending.startTime;
        if (elapsed > OPERATION_TIMEOUT) {
          console.log(`[app] Operation timeout for ${container.name}, clearing pending status`);
          delete pendingOperations.value[container.name];
          return container;
        }
        
        // Check if operation completed (status matches expected state)
        const expectedStates = {
          'stopping': ['exited', 'not-created'],
          'starting': ['running'],
          'restarting': ['running'],
          'creating': ['running']
        };
        
        const expected = expectedStates[pending.status];
        if (expected && expected.includes(container.status)) {
          console.log(`[app] Operation completed for ${container.name}: ${pending.status} → ${container.status}`);
          delete pendingOperations.value[container.name];
          return container;
        }
        
        // Operation still in progress - override with transitional status
        console.log(`[app] Preserving transitional status for ${container.name}: ${pending.status}`);
        return {
          ...container,
          status: pending.status
        };
      });
      
      // Update containers
      data.value.containers = processedContainers;
      data.value.timestamp = message.timestamp;
      
      // Keep existing data for other fields (they're not sent via WebSocket anymore)
      // System metrics and other data can be fetched on-demand if needed
    };

    const handleError = (err) => {
      error.value = err.message || 'WebSocket connection error';
      wsConnected.value = false;
    };

    const handleReconnect = () => {
      console.log('[app] WebSocket reconnected, triggering log re-subscription');
      wsReconnectCount.value++;
    };

    const updateContainerStatus = (update) => {
      console.log('[app] Recording pending operation:', update);
      
      // Record the pending operation with timestamp
      pendingOperations.value = {
        ...pendingOperations.value,
        [update.name]: {
          status: update.status,
          startTime: Date.now()
        }
      };
      
      // Apply immediate optimistic update
      const containerIndex = data.value.containers.findIndex(c => c.name === update.name);
      if (containerIndex !== -1) {
        data.value.containers[containerIndex] = {
          ...data.value.containers[containerIndex],
          status: update.status
        };
        console.log('[app] Container status updated to:', update.status, '- tracking until completion');
      }
    };

    const handleSceneSwitching = (sceneName) => {
      console.log(`[app] handleSceneSwitching called for: ${sceneName}`);
      switchingScene.value = sceneName;
      console.log(`[app] Set switchingScene.value to: ${sceneName}`);
      
      // Set timeout to clear switching state after 5 seconds
      if (switchingTimeout) {
        clearTimeout(switchingTimeout);
      }
      switchingTimeout = setTimeout(() => {
        console.log('[app] Scene switching timeout reached, clearing state');
        switchingScene.value = null;
        switchingTimeout = null;
      }, 5000);
    };

    const toggleInterface = () => {
      interfaceMode.value = interfaceMode.value === 'simplified' ? 'full' : 'simplified';
      localStorage.setItem(STORAGE_KEY, interfaceMode.value);
      console.log(`[app] Switched to ${interfaceMode.value} interface`);
    };

    onMounted(() => {
      console.log('[app] Mounting dashboard...');
      wsService.connect(handleMessage, handleError, handleReconnect);
      
      // Fetch last commit info
      fetchLastCommit();
      
      // Refresh commit data every 5 minutes
      commitRefreshInterval = setInterval(fetchLastCommit, 5 * 60 * 1000);
      
      // Update current time every minute to refresh the "ago" display
      timeUpdateInterval = setInterval(() => {
        currentTime.value = Date.now();
      }, 60 * 1000);
    });

    onUnmounted(() => {
      console.log('[app] Unmounting dashboard...');
      wsService.disconnect();
      if (switchingTimeout) {
        clearTimeout(switchingTimeout);
      }
      if (commitRefreshInterval) {
        clearInterval(commitRefreshInterval);
      }
      if (timeUpdateInterval) {
        clearInterval(timeUpdateInterval);
      }
    });

    return {
      data,
      wsConnected,
      lastUpdate,
      error,
      pollingInterval,
      switchingScene,
      interfaceMode,
      statusWarnings,
      statusSuccess,
      logMessages,
      wsReconnectCount,
      uploadProgress,
      lastCommitTime,
      lastCommitUrl,
      formattedTimeAgo,
      updateContainerStatus,
      handleSceneSwitching,
      toggleInterface
    };
  }
};
</script>

<style>
* {
  margin: 0;
  padding: 0;
  box-sizing: border-box;
}

body {
  font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, 'Helvetica Neue', Arial, sans-serif;
  background: #0f172a;
  color: #e2e8f0;
}

.app {
  min-height: 100vh;
  display: flex;
  flex-direction: column;
}

.header {
  background: #1e293b;
  padding: 20px 30px;
  box-shadow: 0 2px 4px rgba(0, 0, 0, 0.1);
  display: flex;
  justify-content: space-between;
  align-items: center;
  gap: 20px;
}

.header h1 {
  display: inline-block;
  font-size: 1.75rem;
  color: #f1f5f9;
  font-weight: 600;
}

.header-controls {
  display: flex;
  align-items: center;
  gap: 20px;
}

.interface-toggle {
  background: none;
  border: none;
  padding: 0;
  color: #e2e8f0;
  cursor: pointer;
  display: inline-block;
  align-items: center;
}

.toggle-icon {
  font-size: 1.75rem;
  line-height: 1;
}

.connection-status {
  display: flex;
  flex-direction: column;
  align-items: flex-end;
  gap: 4px;
}

.status-indicator {
  font-size: 0.875rem;
  font-weight: 500;
  padding: 6px 12px;
  border-radius: 4px;
}

.status-indicator.connected {
  color: #10b981;
  background: rgba(16, 185, 129, 0.1);
}

.status-indicator.disconnected {
  color: #ef4444;
  background: rgba(239, 68, 68, 0.1);
}

.last-update {
  font-size: 0.75rem;
  color: #64748b;
}

.main-content {
  flex: 1;
  padding: 30px;
  max-width: 1600px;
  width: 100%;
  margin: 0 auto;
}

.error-banner {
  background: rgba(239, 68, 68, 0.1);
  border: 1px solid #ef4444;
  border-radius: 8px;
  padding: 15px;
  margin-bottom: 20px;
  color: #ef4444;
}

.backend-error-banner {
  background: rgba(239, 68, 68, 0.15);
  border: 3px solid #ef4444;
  border-radius: 8px;
  padding: 20px;
  margin-bottom: 20px;
  animation: pulse-border 2s ease-in-out infinite;
}

@keyframes pulse-border {
  0%, 100% {
    border-color: #ef4444;
    box-shadow: 0 0 0 0 rgba(239, 68, 68, 0.4);
  }
  50% {
    border-color: #dc2626;
    box-shadow: 0 0 0 8px rgba(239, 68, 68, 0);
  }
}

.backend-error-content {
  display: flex;
  align-items: flex-start;
  gap: 15px;
}

.backend-error-icon {
  font-size: 2rem;
  flex-shrink: 0;
}

.backend-error-text {
  flex: 1;
}

.backend-error-text strong {
  display: block;
  font-size: 1.2rem;
  color: #ef4444;
  margin-bottom: 8px;
}

.backend-error-text p {
  color: #f87171;
  margin-bottom: 6px;
  line-height: 1.5;
}

.backend-error-text .error-detail {
  font-size: 0.85rem;
  color: #fca5a5;
  font-family: monospace;
  background: rgba(0, 0, 0, 0.2);
  padding: 8px;
  border-radius: 4px;
  margin-top: 10px;
}

.privacy-toggle span {
  display: none;
}

.dashboard-grid {
  display: grid;
  grid-template-columns: 100%;
  gap: 20px;
  margin-bottom: 20px;
}

@media (max-width: 1200px) {
  .dashboard-grid {
    grid-template-columns: 1fr;
  }
}

.containers-section {
  margin-top: 20px;
}

.stream-info {
  margin-bottom: 20px;
}

.status-success {
  background: rgba(16, 185, 129, 0.1);
  border: 2px solid #10b981;
  border-radius: 8px;
  padding: 15px;
  margin-bottom: 15px;
}

.success-item {
  display: flex;
  align-items: center;
  gap: 10px;
  font-size: 1.1rem;
  font-weight: 600;
  color: #10b981;
  padding: 5px 0;
}

.success-icon {
  font-size: 1.5rem;
}

.status-warnings {
  background: rgba(239, 68, 68, 0.1);
  border: 2px solid #ef4444;
  border-radius: 8px;
  padding: 15px;
}

.warning-item {
  display: flex;
  align-items: center;
  gap: 10px;
  font-size: 1.1rem;
  font-weight: 600;
  color: #ef4444;
  padding: 5px 0;
}

.warning-icon {
  font-size: 1.5rem;
}

.footer {
  background: #1e293b;
  padding: 15px 30px;
  text-align: center;
  color: #64748b;
  font-size: 0.875rem;
  border-top: 1px solid #334155;
}

.footer-link {
  color: #94a3b8;
  text-decoration: none;
  transition: color 0.2s ease;
}

.footer-link:hover {
  color: #e2e8f0;
  text-decoration: underline;
}

.footer-loading {
  color: #64748b;
  font-style: italic;
}

@media (max-width: 768px) {
  .header {
    flex-direction: column;
    gap: 15px;
    align-items: flex-start;
  }
  
  .header-controls {
    width: 100%;
    flex-direction: column;
    align-items: flex-start;
  }
  
  .interface-toggle {
    align-self: flex-start;
  }
  
  .connection-status {
    width: 100%;
    align-items: flex-start;
  }
  
  .dashboard-grid {
    grid-template-columns: 1fr;
  }
  
  .main-content {
    padding: 15px;
  }
}
</style>