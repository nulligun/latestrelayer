const axios = require('axios');
const metricsService = require('./metrics');
const ControllerService = require('./controller');

/**
 * Aggregates data from all sources (containers, metrics, compositor)
 */
class AggregatorService {
  constructor(config) {
    this.controllerService = new ControllerService(config.controllerUrl);
    this.compositorUrl = config.compositorUrl;
    this.pollingInterval = config.pollingInterval || 2000;
    this.clients = new Set();
    
    // SRT configuration from environment
    this.srtPort = config.srtPort;
    this.srtDomain = config.srtDomain;
    
    // Stream status tracking
    this.streamStatus = {
      currentScene: null,
      stateChangeTimestamp: Date.now()
    };
  }

  /**
   * Get current scene from compositor
   */
  async getCompositorScene() {
    try {
      const response = await axios.get(`${this.compositorUrl}/scene`, { timeout: 3000 });
      return response.data.scene;
    } catch (error) {
      console.error('[aggregator] Error fetching compositor scene:', error.message);
      return null;
    }
  }

  /**
   * Get privacy mode status from compositor
   */
  async getCompositorPrivacy() {
    try {
      const response = await axios.get(`${this.compositorUrl}/privacy`, { timeout: 3000 });
      return response.data.enabled;
    } catch (error) {
      console.error('[aggregator] Error fetching compositor privacy:', error.message);
      return false;
    }
  }

  /**
   * Aggregate all data from various sources
   */
  async aggregateData() {
    const timestamp = new Date().toISOString();

    try {
      const [containers, systemMetrics, currentScene, privacyEnabled] = await Promise.all([
        this.controllerService.listContainers().catch(err => {
          console.error('[aggregator] Error listing containers:', err.message);
          return [];
        }),
        metricsService.getSystemMetrics(),
        this.getCompositorScene(),
        this.getCompositorPrivacy()
      ]);

      // Track scene changes and update timestamp
      if (currentScene !== this.streamStatus.currentScene) {
        this.streamStatus.currentScene = currentScene;
        this.streamStatus.stateChangeTimestamp = Date.now();
        console.log(`[aggregator] Scene changed: ${currentScene}`);
      }
      
      // Calculate duration in current scene (seconds)
      const sceneDurationSeconds = Math.floor((Date.now() - this.streamStatus.stateChangeTimestamp) / 1000);

      // Determine if compositor is online
      const compositorContainer = containers.find(c => c.name === 'compositor');
      const isOnline = compositorContainer?.running && compositorContainer?.health === 'healthy';
      
      // Map scene to user-friendly names
      const srtConnected = currentScene === 'SRT';

      return {
        timestamp,
        containers: containers.map(c => ({
          name: c.name,
          fullName: c.full_name,
          status: c.status,
          statusDetail: c.status_detail,
          running: c.running,
          health: c.health,
          id: c.id
        })),
        systemMetrics,
        compositorHealth: {
          status: isOnline ? 'healthy' : 'unavailable',
          current_scene: currentScene,
          srt_connected: srtConnected,
          privacy_enabled: privacyEnabled
        },
        currentScene: currentScene,
        streamStatus: {
          isOnline,
          durationSeconds: sceneDurationSeconds,
          srtConnected,
          privacyEnabled
        },
        sceneDurationSeconds,
        cameraConfig: {
          srtUrl: `srt://${this.srtDomain}:${this.srtPort}`
        }
      };
    } catch (error) {
      console.error('[aggregator] Error aggregating data:', error.message);
      
      // Calculate duration even on error
      const durationSeconds = Math.floor((Date.now() - this.streamStatus.stateChangeTimestamp) / 1000);
      
      return {
        timestamp,
        containers: [],
        systemMetrics: { cpu: 0, memory: 0, load: [0] },
        compositorHealth: {
          status: 'error',
          current_scene: 'unknown',
          srt_connected: false,
          privacy_enabled: false
        },
        currentScene: null,
        streamStatus: {
          isOnline: false,
          durationSeconds,
          srtConnected: false,
          privacyEnabled: false
        },
        sceneDurationSeconds: durationSeconds,
        cameraConfig: {
          srtUrl: `srt://${this.srtDomain}:${this.srtPort}`
        },
        error: error.message
      };
    }
  }

  /**
   * Start polling and broadcasting to WebSocket clients
   */
  startPolling(broadcast) {
    console.log(`[aggregator] Starting polling every ${this.pollingInterval}ms`);
    
    const poll = async () => {
      const data = await this.aggregateData();
      broadcast(data);
    };

    // Initial poll
    poll();

    // Set up interval
    this.intervalId = setInterval(poll, this.pollingInterval);
  }

  /**
   * Stop polling
   */
  stopPolling() {
    if (this.intervalId) {
      clearInterval(this.intervalId);
      console.log('[aggregator] Polling stopped');
    }
  }
}

module.exports = AggregatorService;