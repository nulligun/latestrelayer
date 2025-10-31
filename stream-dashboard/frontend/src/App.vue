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
        <SystemMetrics :metrics="data.systemMetrics" />
        <StreamStats :stats="data.rtmpStats" :currentScene="data.currentScene" />
      </div>

      <div class="containers-section">
        <ContainerGrid :containers="data.containers" />
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
      currentScene: null
    });

    const formatTime = (timestamp) => {
      if (!timestamp) return 'Never';
      const date = new Date(timestamp);
      return date.toLocaleTimeString();
    };

    const handleMessage = (newData) => {
      data.value = newData;
      lastUpdate.value = formatTime(newData.timestamp);
      wsConnected.value = true;
      error.value = null;
    };

    const handleError = (err) => {
      error.value = err.message || 'WebSocket connection error';
      wsConnected.value = false;
    };

    onMounted(() => {
      console.log('[app] Mounting dashboard...');
      wsService.connect(handleMessage, handleError);
    });

    onUnmounted(() => {
      console.log('[app] Unmounting dashboard...');
      wsService.disconnect();
    });

    return {
      data,
      wsConnected,
      lastUpdate,
      error,
      pollingInterval
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