<template>
  <div class="app">
    <header class="header">
      <h1>Stream Control Dashboard</h1>
      <div class="connection-status">
        <span class="status-indicator" :class="{ connected: wsConnected, disconnected: !wsConnected }">
          {{ wsConnected ? '● Connected' : '○ Disconnected' }}
        </span>
        <span class="last-update">Last update: {{ lastUpdate }}</span>
      </div>
    </header>

    <main class="main-content">
      <div v-if="error" class="error-banner">
        <strong>Error:</strong> {{ error }}
      </div>

      <div class="dashboard-grid">
        <SystemMetrics
          :metrics="data.systemMetrics"
          :containers="data.containers"
          :rtmpStats="data.rtmpStats"
        />
        <StreamStats
          :stats="data.rtmpStats"
          :streamStatus="data.streamStatus"
          :currentScene="data.currentScene"
          :sceneDurationSeconds="data.sceneDurationSeconds"
          :switchingScene="switchingScene"
          :sourceAvailability="data.sourceAvailability"
          @scene-switching="handleSceneSwitching"
        />
      </div>

      <div class="containers-section">
        <ContainerGrid
          :containers="data.containers"
          @container-status-changed="updateContainerStatus"
        />
      </div>
    </main>

    <footer class="footer">
      <p>Stream Dashboard v1.0 | Polling interval: {{ pollingInterval }}ms</p>
    </footer>
  </div>
</template>

<script>
import { ref, onMounted, onUnmounted } from 'vue';
import { WebSocketService } from './services/websocket.js';
import SystemMetrics from './components/SystemMetrics.vue';
import StreamStats from './components/StreamStats.vue';
import ContainerGrid from './components/ContainerGrid.vue';

export default {
  name: 'App',
  components: {
    SystemMetrics,
    StreamStats,
    ContainerGrid
  },
  setup() {
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
      sceneDurationSeconds: 0
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
      
      data.value = {
        ...newData,
        containers: processedContainers
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
      updateContainerStatus,
      handleSceneSwitching
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
}

.header h1 {
  font-size: 1.75rem;
  color: #f1f5f9;
  font-weight: 600;
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

.dashboard-grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(500px, 1fr));
  gap: 20px;
  margin-bottom: 20px;
}

.containers-section {
  margin-top: 20px;
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
  
  .connection-status {
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