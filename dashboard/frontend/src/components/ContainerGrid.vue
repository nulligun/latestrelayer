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
        
        <div v-if="getStatusDetail(container)" class="container-status-detail">
          {{ getStatusDetail(container) }}
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
import { ref, onMounted, onUnmounted } from 'vue';

export default {
  name: 'ContainerGrid',
  props: {
    containers: {
      type: Array,
      default: () => []
    }
  },
  setup(props, { emit }) {
    const loading = ref(false);
    const actionPending = ref({});
    const actionError = ref({});
    const currentTime = ref(Date.now());
    let timerInterval = null;

    const containerDescriptions = {
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
    };

    // Start the timer that updates every second
    onMounted(() => {
      timerInterval = setInterval(() => {
        currentTime.value = Date.now();
      }, 1000);
    });

    // Clean up the timer when component is unmounted
    onUnmounted(() => {
      if (timerInterval) {
        clearInterval(timerInterval);
      }
    });

    const formatTimeDelta = (milliseconds) => {
      const totalSeconds = Math.floor(milliseconds / 1000);
      
      if (totalSeconds < 60) {
        return `${totalSeconds}s`;
      } else if (totalSeconds < 3600) {
        const minutes = Math.floor(totalSeconds / 60);
        const seconds = totalSeconds % 60;
        return `${minutes}m ${seconds}s`;
      } else if (totalSeconds < 86400) {
        const hours = Math.floor(totalSeconds / 3600);
        const minutes = Math.floor((totalSeconds % 3600) / 60);
        const seconds = totalSeconds % 60;
        return `${hours}h ${minutes}m ${seconds}s`;
      } else {
        const days = Math.floor(totalSeconds / 86400);
        const hours = Math.floor((totalSeconds % 86400) / 3600);
        const minutes = Math.floor((totalSeconds % 3600) / 60);
        return `${days}d ${hours}h ${minutes}m`;
      }
    };

    const getStatusDetail = (container) => {
      try {
        const status = container.status;
        const health = container.health;

        // Debug logging for all containers
        console.log(`[ContainerGrid] getStatusDetail for ${container.name}:`, {
          status,
          health,
          startedAt: container.startedAt,
          finishedAt: container.finishedAt,
          currentTime: currentTime.value
        });

        // For exited containers - only use timestamp if valid
        if (status === 'exited') {
          if (container.finishedAt && container.finishedAt !== null) {
            // Validate timestamp
            const finishedAt = new Date(container.finishedAt);
            const timestampYear = finishedAt.getFullYear();
            
            // Check if timestamp is valid and reasonable (after year 2000, before 2100)
            if (!isNaN(finishedAt.getTime()) && timestampYear >= 2000 && timestampYear < 2100) {
              const elapsed = currentTime.value - finishedAt.getTime();
              
              // Only use calculated time if elapsed is positive and reasonable (less than 10 years)
              if (elapsed >= 0 && elapsed < 315360000000) { // 10 years in milliseconds
                const timeAgo = formatTimeDelta(elapsed);
                
                // Extract exit code from statusDetail if available
                const exitCodeMatch = container.statusDetail?.match(/Exited \((\d+)\)/);
                const exitCode = exitCodeMatch ? exitCodeMatch[1] : '0';
                
                return `Exited (${exitCode}) ${timeAgo} ago`;
              }
            }
          }
          
          // Fall back to static statusDetail for invalid timestamps
          return container.statusDetail;
        }

        // For running containers - only use timestamp if valid
        if (status === 'running') {
          if (container.startedAt && container.startedAt !== null) {
            // Validate timestamp
            const startedAt = new Date(container.startedAt);
            const timestampYear = startedAt.getFullYear();
            
            // Check if timestamp is valid and reasonable (after year 2000, before 2100)
            if (!isNaN(startedAt.getTime()) && timestampYear >= 2000 && timestampYear < 2100) {
              const elapsed = currentTime.value - startedAt.getTime();
              
              // Only use calculated time if elapsed is positive and reasonable (less than 10 years)
              if (elapsed >= 0 && elapsed < 315360000000) { // 10 years in milliseconds
                const uptime = formatTimeDelta(elapsed);

                if (health === 'healthy') {
                  return `Up ${uptime} (healthy)`;
                } else if (health === 'unhealthy') {
                  return `Up ${uptime} (unhealthy)`;
                } else if (health === 'starting') {
                  return `Up ${uptime} (health: starting)`;
                } else {
                  return `Up ${uptime}`;
                }
              }
            }
          }
          
          // Fall back to static statusDetail for invalid timestamps
          return container.statusDetail;
        }

        // For other statuses, always use the static statusDetail
        return container.statusDetail;
      } catch (error) {
        console.error('[ContainerGrid] Error formatting status detail:', error, container);
        return container.statusDetail;
      }
    };

    const isCriticalContainer = (container) => {
      const criticalNames = ['nginx-proxy', 'dashboard', 'controller'];
      return criticalNames.includes(container.name) || container.isCritical === true;
    };

    const getContainerDescription = (name) => {
      return containerDescriptions[name] || 'No description available';
    };

    const isTransitional = (container) => {
      const transitionalStates = ['stopping', 'starting', 'restarting', 'creating'];
      return transitionalStates.includes(container.status);
    };

    const getStatusClass = (container) => {
      // Check for transitional states
      if (isTransitional(container)) {
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
    };

    const getStatusDisplay = (container) => {
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
    };

    const performAction = async (containerName, action) => {
      console.log(`[container] ACTION STARTED: ${action} on ${containerName}`);
      actionPending.value = { ...actionPending.value, [containerName]: action };
      actionError.value = { ...actionError.value, [containerName]: null };
      
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
        emit('container-status-changed', {
          name: containerName,
          status: data.status
        });
      } catch (error) {
        console.error(`[container] ${action} error:`, error);
        actionError.value = { ...actionError.value, [containerName]: error.message };
      } finally {
        actionPending.value = { ...actionPending.value, [containerName]: null };
        console.log(`[container] ACTION COMPLETED: actionPending cleared for ${containerName}`);
      }
    };

    const startContainer = (name) => {
      performAction(name, 'start');
    };

    const stopContainer = (name) => {
      performAction(name, 'stop');
    };

    const restartContainer = (name) => {
      performAction(name, 'restart');
    };

    const createAndStartContainer = (name) => {
      performAction(name, 'create-and-start');
    };

    return {
      loading,
      actionPending,
      actionError,
      currentTime,
      isCriticalContainer,
      getContainerDescription,
      isTransitional,
      getStatusClass,
      getStatusDisplay,
      getStatusDetail,
      performAction,
      startContainer,
      stopContainer,
      restartContainer,
      createAndStartContainer
    };
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