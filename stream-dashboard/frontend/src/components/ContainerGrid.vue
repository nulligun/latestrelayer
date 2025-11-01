<template>
  <div class="containers-card">
    <h2>Container Controls</h2>
    <div v-if="loading" class="loading">Loading containers...</div>
    <div v-else class="containers-grid">
      <div v-for="container in containers" :key="container.name" class="container-item">
        <div class="container-header">
          <div class="container-name">{{ container.name }}</div>
          <div class="container-status" :class="getStatusClass(container.status)">
            <span class="status-dot">●</span>
            {{ container.status.toUpperCase() }}
          </div>
        </div>
        
        <div class="container-description">{{ getContainerDescription(container.name) }}</div>
        
        <div class="container-actions">
          <button 
            @click="startContainer(container.name)"
            :disabled="container.running || actionPending[container.name]"
            class="btn btn-start"
          >
            {{ actionPending[container.name] === 'start' ? 'Starting...' : 'Start' }}
          </button>
          
          <button 
            @click="stopContainer(container.name)"
            :disabled="!container.running || actionPending[container.name]"
            class="btn btn-stop"
          >
            {{ actionPending[container.name] === 'stop' ? 'Stopping...' : 'Stop' }}
          </button>
          
          <button 
            @click="restartContainer(container.name)"
            :disabled="actionPending[container.name]"
            class="btn btn-restart"
          >
            {{ actionPending[container.name] === 'restart' ? 'Restarting...' : 'Restart' }}
          </button>
        </div>
        
        <div v-if="actionError[container.name]" class="error-message">
          {{ actionError[container.name] }}
        </div>
      </div>
    </div>
  </div>
</template>

<script>
export default {
  name: 'ContainerGrid',
  props: {
    containers: {
      type: Array,
      default: () => []
    }
  },
  data() {
    return {
      loading: false,
      actionPending: {},
      actionError: {},
      containerDescriptions: {
        'ffmpeg-kick': 'Stream to kick',
        'stream-switcher': 'Mux Offline and Cam to Program',
        'dashboard': 'This control panel',
        'ffmpeg-offline': 'A looping video of the BRB screen',
        'ffmpeg-dev-cam': 'A looping video for testing the camera feed',
        'nginx-rtmp': 'RTMP Relay server',
        'controller': 'API for container management'
      }
    };
  },
  methods: {
    getContainerDescription(name) {
      return this.containerDescriptions[name] || 'No description available';
    },
    getStatusClass(status) {
      if (status === 'running') return 'status-running';
      if (status === 'exited') return 'status-stopped';
      return 'status-unknown';
    },
    
    async performAction(containerName, action) {
      this.actionPending = { ...this.actionPending, [containerName]: action };
      this.actionError = { ...this.actionError, [containerName]: null };
      
      try {
        const response = await fetch(`/api/container/${containerName}/${action}`, {
          method: 'POST',
          headers: {
            'Content-Type': 'application/json'
          }
        });
        
        const data = await response.json();
        
        if (!response.ok) {
          throw new Error(data.error || `Failed to ${action} container`);
        }
        
        console.log(`[container] ${action} success:`, data);
      } catch (error) {
        console.error(`[container] ${action} error:`, error);
        this.actionError = { ...this.actionError, [containerName]: error.message };
      } finally {
        this.actionPending = { ...this.actionPending, [containerName]: null };
      }
    },
    
    startContainer(name) {
      this.performAction(name, 'start');
    },
    
    stopContainer(name) {
      this.performAction(name, 'stop');
    },
    
    restartContainer(name) {
      this.performAction(name, 'restart');
    }
  }
};
</script>

<style scoped>
.containers-card {
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

.loading {
  text-align: center;
  padding: 40px;
  color: #94a3b8;
}

.containers-grid {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(280px, 1fr));
  gap: 15px;
}

.container-item {
  background: #0f172a;
  border-radius: 6px;
  padding: 15px;
  border: 1px solid #334155;
}

.container-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 10px;
  padding-bottom: 10px;
  border-bottom: 1px solid #334155;
}

.container-name {
  font-weight: bold;
  color: #f1f5f9;
  font-size: 0.95rem;
}

.container-status {
  display: flex;
  align-items: center;
  gap: 6px;
  font-size: 0.75rem;
  font-weight: bold;
  padding: 4px 10px;
  border-radius: 4px;
}

.status-dot {
  font-size: 0.6rem;
}

.status-running {
  color: #10b981;
  background: rgba(16, 185, 129, 0.1);
}

.status-stopped {
  color: #ef4444;
  background: rgba(239, 68, 68, 0.1);
}

.status-unknown {
  color: #64748b;
  background: rgba(100, 116, 139, 0.1);
}

.container-description {
  font-size: 0.8rem;
  color: #94a3b8;
  margin-bottom: 15px;
  line-height: 1.4;
  font-style: italic;
}

.container-actions {
  display: flex;
  gap: 8px;
}

.btn {
  flex: 1;
  padding: 8px 12px;
  border: none;
  border-radius: 4px;
  font-size: 0.875rem;
  font-weight: 500;
  cursor: pointer;
  transition: all 0.2s;
}

.btn:disabled {
  opacity: 0.5;
  cursor: not-allowed;
}

.btn-start {
  background: #10b981;
  color: white;
}

.btn-start:hover:not(:disabled) {
  background: #059669;
}

.btn-stop {
  background: #ef4444;
  color: white;
}

.btn-stop:hover:not(:disabled) {
  background: #dc2626;
}

.btn-restart {
  background: #3b82f6;
  color: white;
}

.btn-restart:hover:not(:disabled) {
  background: #2563eb;
}

.error-message {
  margin-top: 10px;
  padding: 8px;
  background: rgba(239, 68, 68, 0.1);
  border: 1px solid #ef4444;
  border-radius: 4px;
  color: #ef4444;
  font-size: 0.75rem;
}
</style>