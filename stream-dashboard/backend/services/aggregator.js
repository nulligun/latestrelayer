const axios = require('axios');
const metricsService = require('./metrics');
const ControllerService = require('./controller');

/**
 * Aggregates data from all sources (containers, metrics, srt-switcher)
 */
class AggregatorService {
  constructor(config) {
    this.controllerService = new ControllerService(config.controllerUrl);
    this.switcherUrl = config.switcherUrl;
    this.pollingInterval = config.pollingInterval || 2000;
    this.clients = new Set();
    
    // SRT configuration from environment
    this.srtPort = config.srtPort;
    this.srtDomain = config.srtDomain;
    
    // Stream status tracking
    this.streamStatus = {
      currentSource: null,
      srtConnected: false,
      stateChangeTimestamp: Date.now()
    };
  }

  /**
   * Get data from srt-switcher health endpoint
   */
  async getSwitcherHealth() {
    try {
      const response = await axios.get(`${this.switcherUrl}/health`, { timeout: 3000 });
      return response.data;
    } catch (error) {
      console.error('[aggregator] Error fetching srt-switcher health:', error.message);
      return null;
    }
  }

  /**
   * Aggregate all data from various sources
   */
  async aggregateData() {
    const timestamp = new Date().toISOString();

    try {
      const [containers, systemMetrics, switcherHealth] = await Promise.all([
        this.controllerService.listContainers().catch(err => {
          console.error('[aggregator] Error listing containers:', err.message);
          return [];
        }),
        metricsService.getSystemMetrics(),
        this.getSwitcherHealth()
      ]);

      // Get current state from srt-switcher
      const currentSource = switcherHealth?.current_source || 'unknown';
      const srtConnected = switcherHealth?.srt_connected || false;
      
      // Track state changes and update timestamp
      if (currentSource !== this.streamStatus.currentSource || 
          srtConnected !== this.streamStatus.srtConnected) {
        this.streamStatus.currentSource = currentSource;
        this.streamStatus.srtConnected = srtConnected;
        this.streamStatus.stateChangeTimestamp = Date.now();
        console.log(`[aggregator] State changed: source=${currentSource}, srt_connected=${srtConnected}`);
      }
      
      // Calculate duration in current state (seconds)
      const durationSeconds = Math.floor((Date.now() - this.streamStatus.stateChangeTimestamp) / 1000);

      // Determine if stream is online (srt-switcher is running and pipeline is playing)
      const switcherContainer = containers.find(c => c.name === 'srt-switcher');
      const isOnline = switcherContainer?.running && switcherHealth?.status === 'healthy';
      
      // Get kick streaming state from switcher health
      const kickStreamingEnabled = switcherHealth?.kick_streaming_enabled || false;

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
        switcherHealth: switcherHealth || {
          status: 'unavailable',
          current_source: 'unknown',
          srt_connected: false,
          kick_streaming_enabled: false
        },
        currentScene: currentSource,
        streamStatus: {
          isOnline,
          durationSeconds,
          srtConnected,
          kickStreamingEnabled
        },
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
        switcherHealth: {
          status: 'error',
          current_source: 'unknown',
          srt_connected: false
        },
        currentScene: null,
        streamStatus: {
          isOnline: false,
          durationSeconds,
          srtConnected: false
        },
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