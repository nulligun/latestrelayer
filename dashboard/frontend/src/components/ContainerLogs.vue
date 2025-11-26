<template>
  <div class="logs-card">
    <h2>Container Logs</h2>
    
    <div v-if="loading" class="loading">Loading logs...</div>
    
    <div v-else class="logs-content">
      <!-- Tab Navigation -->
      <div class="tabs">
        <button
          v-for="container in containers"
          :key="container.name"
          @click="activeTab = container.name"
          :class="['tab', { active: activeTab === container.name }]"
        >
          {{ container.name }}
          <span v-if="container.status === 'running'" class="tab-indicator running">●</span>
          <span v-else class="tab-indicator stopped">○</span>
        </button>
      </div>

      <!-- Filter Input -->
      <div class="filter-section">
        <input
          v-model="filterText"
          type="text"
          placeholder="Filter logs (case-insensitive)..."
          class="filter-input"
        />
        <span v-if="filterText" class="filter-count">
          {{ filteredLogs.length }} / {{ currentLogs.length }} lines
        </span>
      </div>

      <!-- Logs Display -->
      <div class="logs-display" ref="logsContainer" @scroll="handleScroll">
        <div v-if="currentLogs.length === 0" class="no-logs">
          No logs available for this container
        </div>
        <div v-else>
          <div
            v-for="(log, index) in filteredLogs"
            :key="index"
            class="log-line"
          >
            {{ log }}
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<script>
import { inject } from 'vue';

export default {
  name: 'ContainerLogs',
  props: {
    containers: {
      type: Array,
      default: () => []
    },
    logMessages: {
      type: Array,
      default: () => []
    },
    wsReconnectCount: {
      type: Number,
      default: 0
    }
  },
  setup() {
    // Get WebSocket service from parent for sending subscription messages
    const wsService = inject('wsService', null);
    return { wsService };
  },
  data() {
    return {
      loading: false,
      activeTab: null,
      filterText: '',
      logs: {}, // Store logs per container: { containerName: [log lines] }
      isUserScrolled: false, // Track if user has manually scrolled up
      messageHandler: null
    };
  },
  computed: {
    currentLogs() {
      if (!this.activeTab || !this.logs[this.activeTab]) {
        return [];
      }
      const logs = this.logs[this.activeTab];
      console.log(`[logs] currentLogs computed - ${this.activeTab}: ${logs.length} lines`);
      return logs;
    },
    filteredLogs() {
      if (!this.filterText) {
        const result = this.currentLogs;
        console.log(`[logs] filteredLogs computed (no filter) - ${result.length} lines`);
        return result;
      }
      const filterLower = this.filterText.toLowerCase();
      const result = this.currentLogs.filter(log =>
        log.toLowerCase().includes(filterLower)
      );
      console.log(`[logs] filteredLogs computed (filtered) - ${result.length}/${this.currentLogs.length} lines`);
      return result;
    }
  },
  watch: {
    containers: {
      immediate: true,
      handler(newContainers) {
        // Set first container as active tab if not set
        if (newContainers.length > 0 && !this.activeTab) {
          this.activeTab = newContainers[0].name;
        }
      }
    },
    logMessages: {
      handler(messages) {
        // Process the latest message
        if (messages.length > 0) {
          const latestMessage = messages[messages.length - 1];
          console.log(`[logs] Received message from App.vue:`, latestMessage.type, `for`, latestMessage.container);
          
          if (latestMessage.type === 'log_snapshot') {
            this.handleLogSnapshot(latestMessage);
          } else if (latestMessage.type === 'new_logs') {
            this.handleNewLogs(latestMessage);
          }
        }
      },
      deep: true
    },
    activeTab(newTab, oldTab) {
      // Unsubscribe from old tab
      if (oldTab && this.wsService) {
        this.unsubscribeFromLogs(oldTab);
      }
      
      // Subscribe to new tab
      if (newTab && this.wsService) {
        this.subscribeToLogs(newTab);
      }
      
      // Reset scroll tracking when changing tabs
      this.isUserScrolled = false;
      this.$nextTick(() => {
        this.scrollToBottom();
      });
    },
    wsReconnectCount(newVal, oldVal) {
      // Re-subscribe to logs when WebSocket reconnects
      if (newVal > oldVal && this.activeTab) {
        console.log(`[logs] WebSocket reconnected, re-subscribing to ${this.activeTab}`);
        this.subscribeToLogs(this.activeTab);
      }
    },
    filteredLogs() {
      // Auto-scroll to bottom when new logs arrive, but only if user hasn't scrolled up
      this.$nextTick(() => {
        if (!this.isUserScrolled) {
          this.scrollToBottom();
        }
      });
    }
  },
  mounted() {
    console.log('[logs] Component mounted');
    
    // Subscribe to logs for active tab
    if (this.activeTab) {
      this.subscribeToLogs(this.activeTab);
    }
  },
  unmounted() {
    console.log('[logs] Component unmounting, cleaning up WebSocket subscriptions');
    
    // Unsubscribe from active tab
    if (this.activeTab) {
      this.unsubscribeFromLogs(this.activeTab);
    }
  },
  methods: {
    subscribeToLogs(containerName) {
      if (!this.wsService || !this.wsService.isConnected()) {
        console.warn('[logs] Cannot subscribe - WebSocket not connected');
        return;
      }
      
      console.log(`[logs] Subscribing to logs for ${containerName}`);
      
      const message = {
        type: 'subscribe_logs',
        container: containerName,
        lines: 100
      };
      
      this.wsService.ws.send(JSON.stringify(message));
    },
    
    unsubscribeFromLogs(containerName) {
      if (!this.wsService || !this.wsService.isConnected()) {
        return;
      }
      
      console.log(`[logs] Unsubscribing from logs for ${containerName}`);
      
      const message = {
        type: 'unsubscribe_logs',
        container: containerName
      };
      
      this.wsService.ws.send(JSON.stringify(message));
    },
    
    handleLogSnapshot(message) {
      const { container, logs } = message;
      console.log(`[logs] Received log snapshot for ${container}: ${logs?.length} lines`);
      
      // Store logs with rotation (keep max 500 lines)
      let newLogs = logs || [];
      if (newLogs.length > 500) {
        newLogs = newLogs.slice(-500);
      }
      
      console.log(`[logs] Setting ${newLogs.length} logs for ${container}`);
      
      // Use completely new object for reactivity
      const updatedLogs = { ...this.logs };
      updatedLogs[container] = newLogs;
      this.logs = updatedLogs;
      
      console.log(`[logs] Snapshot stored, currentLogs.length:`, this.currentLogs.length);
    },
    
    handleNewLogs(message) {
      const { container, logs } = message;
      
      console.log(`[logs] handleNewLogs called for ${container}`, {
        receivedLogCount: logs?.length,
        messageType: message.type
      });
      
      if (!logs || logs.length === 0) {
        console.log(`[logs] No logs in message for ${container}`);
        return;
      }
      
      // Append new logs to existing ones
      const existingLogs = this.logs[container] || [];
      console.log(`[logs] Before update - ${container} has ${existingLogs.length} logs`);
      
      let updatedLogs = [...existingLogs, ...logs];
      
      // Keep only last 500 lines
      if (updatedLogs.length > 500) {
        updatedLogs = updatedLogs.slice(-500);
      }
      
      console.log(`[logs] After update - ${container} will have ${updatedLogs.length} logs`);
      
      // Use Vue.set equivalent for Vue 3 reactivity
      // Creating a completely new object to ensure reactivity
      const newLogs = { ...this.logs };
      newLogs[container] = updatedLogs;
      this.logs = newLogs;
      
      console.log(`[logs] Logs object updated, currentLogs.length:`, this.currentLogs.length);
    },
    
    handleScroll() {
      const container = this.$refs.logsContainer;
      if (!container) return;
      
      // Check if user is at the bottom (within 50px tolerance)
      const isAtBottom = container.scrollHeight - container.scrollTop - container.clientHeight < 50;
      this.isUserScrolled = !isAtBottom;
    },
    
    scrollToBottom() {
      const container = this.$refs.logsContainer;
      if (container) {
        container.scrollTop = container.scrollHeight;
      }
    }
  }
};
</script>

<style scoped>
.logs-card {
  background: #1e293b;
  border-radius: 8px;
  padding: 20px;
  box-shadow: 0 4px 6px rgba(0, 0, 0, 0.1);
  margin-top: 20px;
}

h2 {
  margin: 0 0 20px 0;
  font-size: 1.25rem;
  color: #f1f5f9;
}

.loading {
  text-align: center;
  padding: 40px;
  color: #94a3b8;
}

.logs-content {
  display: flex;
  flex-direction: column;
  gap: 15px;
}

/* Tabs */
.tabs {
  display: flex;
  gap: 8px;
  flex-wrap: wrap;
  border-bottom: 1px solid #334155;
  padding-bottom: 10px;
}

.tab {
  padding: 8px 16px;
  background: #0f172a;
  border: 1px solid #334155;
  border-radius: 6px 6px 0 0;
  color: #94a3b8;
  cursor: pointer;
  transition: all 0.2s;
  font-size: 0.875rem;
  display: flex;
  align-items: center;
  gap: 6px;
}

.tab:hover {
  background: #1e293b;
  color: #e2e8f0;
}

.tab.active {
  background: #334155;
  color: #f1f5f9;
  border-bottom-color: #334155;
}

.tab-indicator {
  font-size: 0.5rem;
}

.tab-indicator.running {
  color: #10b981;
}

.tab-indicator.stopped {
  color: #64748b;
}

/* Filter Section */
.filter-section {
  display: flex;
  align-items: center;
  gap: 10px;
}

.filter-input {
  flex: 1;
  padding: 10px 15px;
  background: #0f172a;
  border: 1px solid #334155;
  border-radius: 6px;
  color: #e2e8f0;
  font-size: 0.875rem;
  outline: none;
  transition: border-color 0.2s;
}

.filter-input:focus {
  border-color: #3b82f6;
}

.filter-input::placeholder {
  color: #64748b;
}

.filter-count {
  font-size: 0.75rem;
  color: #64748b;
  white-space: nowrap;
}

/* Logs Display */
.logs-display {
  background: #0f172a;
  border: 1px solid #334155;
  border-radius: 6px;
  padding: 15px;
  height: 500px;
  overflow-y: auto;
  font-family: 'Consolas', 'Monaco', 'Courier New', monospace;
  font-size: 0.75rem;
  line-height: 1.5;
}

.no-logs {
  color: #64748b;
  text-align: center;
  padding: 40px;
  font-style: italic;
}

.log-line {
  color: #e2e8f0;
  white-space: pre-wrap;
  word-break: break-all;
  padding: 2px 0;
}

.log-line:hover {
  background: rgba(59, 130, 246, 0.1);
}

/* Custom scrollbar */
.logs-display::-webkit-scrollbar {
  width: 8px;
}

.logs-display::-webkit-scrollbar-track {
  background: #1e293b;
  border-radius: 4px;
}

.logs-display::-webkit-scrollbar-thumb {
  background: #475569;
  border-radius: 4px;
}

.logs-display::-webkit-scrollbar-thumb:hover {
  background: #64748b;
}
</style>