const axios = require('axios');
const metricsService = require('./metrics');
const ControllerService = require('./controller');
const RtmpParserService = require('./rtmpParser');

/**
 * Aggregates data from all sources (containers, metrics, RTMP stats, scene)
 */
class AggregatorService {
  constructor(config) {
    this.controllerService = new ControllerService(config.controllerUrl);
    this.rtmpParser = new RtmpParserService(config.nginxStatsUrl);
    this.switcherUrl = config.switcherUrl;
    this.pollingInterval = config.pollingInterval || 2000;
    this.clients = new Set();
  }

  /**
   * Get current scene from stream-switcher
   */
  async getCurrentScene() {
    try {
      // The stream-switcher doesn't have a specific endpoint for current scene,
      // so we'll need to infer it or add one. For now, return null
      // This can be enhanced by adding a /status endpoint to stream-switcher
      return null;
    } catch (error) {
      console.error('[aggregator] Error getting current scene:', error.message);
      return null;
    }
  }

  /**
   * Aggregate all data from various sources
   */
  async aggregateData() {
    const timestamp = new Date().toISOString();

    try {
      const [containers, systemMetrics, rtmpStats, currentScene] = await Promise.all([
        this.controllerService.listContainers(),
        metricsService.getSystemMetrics(),
        this.rtmpParser.getStats(),
        this.getCurrentScene()
      ]);

      return {
        timestamp,
        containers: containers.map(c => ({
          name: c.name,
          fullName: c.full_name,
          status: c.status,
          running: c.running,
          id: c.id
        })),
        systemMetrics,
        rtmpStats,
        currentScene
      };
    } catch (error) {
      console.error('[aggregator] Error aggregating data:', error.message);
      return {
        timestamp,
        containers: [],
        systemMetrics: { cpu: 0, memory: 0, load: [0] },
        rtmpStats: { inboundBandwidth: 0, streams: {} },
        currentScene: null,
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