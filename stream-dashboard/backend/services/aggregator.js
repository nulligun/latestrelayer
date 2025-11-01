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
    this.rtmpParser = new RtmpParserService(config.nginxStatsUrl, config.switcherUrl);
    this.switcherUrl = config.switcherUrl;
    this.pollingInterval = config.pollingInterval || 2000;
    this.clients = new Set();
    
    // Stream status tracking
    this.streamStatus = {
      isOnline: false,
      stateChangeTimestamp: Date.now()
    };
  }

  /**
   * Aggregate all data from various sources
   */
  async aggregateData() {
    const timestamp = new Date().toISOString();

    try {
      const [containers, systemMetrics, rtmpStats] = await Promise.all([
        this.controllerService.listContainers(),
        metricsService.getSystemMetrics(),
        this.rtmpParser.getStats()
      ]);

      // Check if ffmpeg-kick is running to determine stream status
      const ffmpegKick = containers.find(c => c.name === 'ffmpeg-kick');
      const isOnline = ffmpegKick ? ffmpegKick.running : false;
      
      // Track state changes and update timestamp
      if (isOnline !== this.streamStatus.isOnline) {
        this.streamStatus.isOnline = isOnline;
        this.streamStatus.stateChangeTimestamp = Date.now();
        console.log(`[aggregator] Stream status changed to ${isOnline ? 'ONLINE' : 'OFFLINE'}`);
      }
      
      // Calculate duration in current state (seconds)
      const durationSeconds = Math.floor((Date.now() - this.streamStatus.stateChangeTimestamp) / 1000);

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
        currentScene: rtmpStats.currentScene,
        streamStatus: {
          isOnline,
          durationSeconds
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
        rtmpStats: { inboundBandwidth: 0, streams: {} },
        currentScene: null,
        streamStatus: {
          isOnline: this.streamStatus.isOnline,
          durationSeconds
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