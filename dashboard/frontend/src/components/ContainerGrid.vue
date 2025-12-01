<template>
  <div class="containers-card">
    <h2>Container Controls</h2>
    <div v-if="loading" class="loading">Loading containers...</div>
    <div v-else class="containers-grid">
      <div v-for="container in containers" :key="container.name" class="container-item">
        <div class="container-header">
          <div class="container-name">
            {{ container.name }}
          </div>
          <div class="container-status" :class="getStatusClass(container)">
            <span v-if="isTransitional(container)" class="status-spinner">⟳</span>
            <span v-else class="status-dot">●</span>
            {{ getStatusDisplay(container) }}
          </div>
        </div>
        
        <div class="container-description">{{ getContainerDescription(container.name) }}</div>
        
        <div v-if="container.statusDetail" class="container-status-detail">
          {{ container.statusDetail }}
        </div>
        
        <div class="container-actions">
          <template v-if="container.status === 'not-created'">
            <button
              @click="createAndStartContainer(container.name)"
              :disabled="actionPending[container.name] || isTransitional(container)"
              class="btn btn-create"
            >
              {{ actionPending[container.name] === 'create-and-start' ? 'Creating...' : 'Create & Start' }}
            </button>
          </template>
          <template v-else-if="container.name === 'controller' || container.name === 'dashboard' || container.name === 'nginx-proxy'">
            <button
              @click="restartContainer(container.name)"
              :disabled="actionPending[container.name] || isTransitional(container) || container.status === 'unknown'"
              class="btn btn-restart"
            >
              {{ actionPending[container.name] === 'restart' ? 'Restarting...' : 'Restart' }}
            </button>
          </template>
          <template v-else>
            <button
              @click="startContainer(container.name)"
              :disabled="container.running || actionPending[container.name] || isTransitional(container)"
              class="btn btn-start"
            >
              {{ actionPending[container.name] === 'start' ? 'Starting...' : 'Start' }}
            </button>
            
            <button
              @click="stopContainer(container.name)"
              :disabled="!container.running || actionPending[container.name] || isTransitional(container)"
              class="btn btn-stop"
            >
              {{ actionPending[container.name] === 'stop' ? 'Stopping...' : 'Stop' }}
            </button>
          </template>
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
        'multiplexer': 'Auto switch between Camera and BRB',
        'ffmpeg-kick': 'Stream to kick',
        'muxer': 'Mux BRB and Cam to Program',
        'dashboard': 'This control panel',
        'ffmpeg-fallback': 'A looping video of the BRB screen',
        'mock-camera': 'A looping video for testing the camera feed',
        'nginx-rtmp': 'RTMP Relay server',
        'ffmpeg-srt-live': 'SRT to RTMP relay',
        'controller': 'API for container management',
        'nginx-proxy': 'Nginx reverse proxy - provides HTTPS access to dashboard'
      }
    };
  },
  methods: {
    isCriticalContainer(container) {
      const criticalNames = ['nginx-proxy', 'dashboard', 'controller'];
      return criticalNames.includes(container.name) || container.isCritical === true;
    },
    getContainerDescription(name) {
      return this.containerDescriptions[name] || 'No description available';
    },
    isTransitional(container) {
      const transitionalStates = ['stopping', 'starting', 'restarting', 'creating'];
      return transitionalStates.includes(container.status);
    },
    getStatusClass(container) {
      // Check for transitional states
      if (this.isTransitional(container)) {
        return 'status-transitional';
      }
      // Check for unknown status (controller API unavailable)
      if (container.status === 'unknown') {
        return 'status-unknown';
      }
      // Check if running but unhealthy
      if (container.status === 'running' && container.health === 'unhealthy') {
        return 'status-unhealthy';
      }
      if (container.status === 'running') return 'status-running';
      if (container.status === 'exited') return 'status-not-running';
      if (container.status === 'not-created') return 'status-not-created';
      return 'status-unknown';
    },
    getStatusDisplay(container) {
      // Handle transitional states
      if (container.status === 'stopping') return 'STOPPING';
      if (container.status === 'starting') return 'STARTING';
      if (container.status === 'restarting') return 'RESTARTING';
      if (container.status === 'creating') return 'CREATING';
      // Show UNKNOWN if status cannot be determined (API unavailable)
      if (container.status === 'unknown') return 'STATUS UNKNOWN';
      // Show UNHEALTHY if running but unhealthy
      if (container.status === 'running' && container.health === 'unhealthy') {
        return 'UNHEALTHY';
      }
      if (container.status === 'exited') return 'NOT-RUNNING';
      if (container.status === 'not-created') return 'NOT-CREATED';
      return container.status.toUpperCase();
    },
    
    async performAction(containerName, action) {
      console.log(`[container] ACTION STARTED: ${action} on ${containerName}`);
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
        console.log(`[container] RESPONSE received:`, {
          status: response.status,
          data: data
        });
        
        if (!response.ok) {
          throw new Error(data.error || `Failed to ${action} container`);
        }
        
        console.log(`[container] ${action} success - transitional status:`, data.status);
        
        // DIAGNOSTIC: Emit event to parent to immediately update container status
        this.$emit('container-status-changed', {
          name: containerName,
          status: data.status
        });
      } catch (error) {
        console.error(`[container] ${action} error:`, error);
        this.actionError = { ...this.actionError, [containerName]: error.message };
      } finally {
        this.actionPending = { ...this.actionPending, [containerName]: null };
        console.log(`[container] ACTION COMPLETED: actionPending cleared for ${containerName}`);
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
    },
    
    createAndStartContainer(name) {
      this.performAction(name, 'create-and-start');
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
  display: flex;
  align-items: center;
  gap: 8px;
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

.status-not-running {
  color: #f59e0b;
  background: rgba(245, 158, 11, 0.1);
}

.status-not-created {
  color: #f59e0b;
  background: rgba(245, 158, 11, 0.1);
}

.status-unhealthy {
  color: #f59e0b;
  background: rgba(245, 158, 11, 0.1);
}

.status-transitional {
  color: #f59e0b;
  background: rgba(245, 158, 11, 0.1);
}

.status-unknown {
  color: #f59e0b;
  background: rgba(245, 158, 11, 0.15);
  border: 1px solid #f59e0b;
}

.status-spinner {
  display: inline-block;
  font-size: 0.9rem;
  animation: spin 1s linear infinite;
}

@keyframes spin {
  from {
    transform: rotate(0deg);
  }
  to {
    transform: rotate(360deg);
  }
}

.container-description {
  font-size: 0.8rem;
  color: #94a3b8;
  margin-bottom: 10px;
  line-height: 1.4;
  font-style: italic;
}

.container-status-detail {
  font-size: 0.75rem;
  color: #64748b;
  margin-bottom: 15px;
  padding: 6px 10px;
  background: rgba(100, 116, 139, 0.1);
  border-radius: 4px;
  border-left: 2px solid #475569;
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

.btn-create {
  background: #f59e0b;
  color: white;
  flex: 1;
}

.btn-create:hover:not(:disabled) {
  background: #d97706;
}

.btn-restart {
  background: #06b6d4;
  color: white;
  flex: 1;
}

.btn-restart:hover:not(:disabled) {
  background: #0891b2;
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