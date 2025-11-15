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

      <!-- Simplified Interface -->
      <SimplifiedView
        v-if="interfaceMode === 'simplified'"
        :containers="data.containers"
        :currentScene="data.currentScene"
        :sceneDurationSeconds="data.sceneDurationSeconds"
        :rtmpStats="data.rtmpStats"
        :switcherHealth="data.switcherHealth"
        :streamStatus="data.streamStatus"
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
          />
          <StreamControls
            :switcherHealth="data.switcherHealth"
            :fallbackConfig="data.fallbackConfig"
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
          />
        </div>
      </template>
    </main>

    <footer class="footer">
      <p>Latest Relayer v1.0 | Polling interval: {{ pollingInterval }}ms</p>
    </footer>
  </div>
</template>

<script>
import { ref, computed, onMounted, onUnmounted } from 'vue';
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
      
      const camStream = data.value.rtmpStats?.streams?.cam;
      if (!camStream || !camStream.active || !camStream.publishing) {
        warnings.push('CAMERA NOT CONNECTED');
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
    
    const data = ref({
      timestamp: null,
      containers: [],
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
        kick_streaming_enabled: false
      },
      fallbackConfig: {
        source: 'BLACK',
        imagePath: '/app/shared/offline.png',
        videoPath: '/app/shared/offline.mp4',
        browserUrl: 'https://example.com'
      }
    });

    const formatTime = (timestamp) => {
      if (!timestamp) return 'Never';
      const date = new Date(timestamp);
      return date.toLocaleTimeString();
    };

    const handleMessage = (newData) => {
      const now = Date.now();
      const OPERATION_TIMEOUT = 30000; // 30 seconds
      
      // Detect scene changes and clear switching state
      if (newData.currentScene !== previousScene && switchingScene.value) {
        console.log(`[app] Scene changed from ${previousScene} to ${newData.currentScene}, clearing switching state`);
        switchingScene.value = null;
        if (switchingTimeout) {
          clearTimeout(switchingTimeout);
          switchingTimeout = null;
        }
      }
      previousScene = newData.currentScene;
      
      // Process containers and apply pending operations
      const processedContainers = newData.containers.map(container => {
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
      
      // Map compositorHealth to switcherHealth for component compatibility
      data.value = {
        ...newData,
        containers: processedContainers,
        switcherHealth: newData.compositorHealth || data.value.switcherHealth
      };
      lastUpdate.value = formatTime(newData.timestamp);
      wsConnected.value = true;
      error.value = null;
    };

    const handleError = (err) => {
      error.value = err.message || 'WebSocket connection error';
      wsConnected.value = false;
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
      wsService.connect(handleMessage, handleError);
    });

    onUnmounted(() => {
      console.log('[app] Unmounting dashboard...');
      wsService.disconnect();
      if (switchingTimeout) {
        clearTimeout(switchingTimeout);
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