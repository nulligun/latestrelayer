const fs = require('fs').promises;
const path = require('path');
const http = require('http');
const https = require('https');
const metricsService = require('./metrics');
const ControllerService = require('./controller');

/**
 * Critical containers that must always be visible in the dashboard
 * These are restart-only containers that are essential for the system to function
 */
const CRITICAL_CONTAINERS = [
  {
    name: 'nginx-proxy',
    fullName: 'latestrelayer-nginx-proxy',
    description: 'Provides HTTPS access to dashboard'
  },
  {
    name: 'dashboard',
    fullName: 'latestrelayer-dashboard',
    description: 'This control panel'
  },
  {
    name: 'controller',
    fullName: 'latestrelayer-controller',
    description: 'API for container management'
  }
];

/**
 * Aggregates data from all sources (containers, metrics, compositor)
 */
class AggregatorService {
  constructor(config) {
    this.controllerService = new ControllerService(config.controllerUrl);
    this.compositorUrl = config.compositorUrl;
    this.pollingInterval = config.pollingInterval || 2000;
    this.clients = new Set();
    
    // Controller WebSocket client for scene/privacy state
    this.controllerWs = config.controllerWs;
    
    // SRT configuration from environment
    this.srtPort = config.srtPort;
    this.srtDomain = config.srtDomain;
    
    // Fallback config path
    this.fallbackConfigPath = '/app/shared/fallback_config.json';
    
    // Stream status tracking - now using controller state via WebSocket
    this.streamStatus = {
      currentScene: 'unknown',
      stateChangeTimestamp: Date.now()
    };
    
    // Track kick streaming status for logging changes
    this.lastKickStatus = false;
    
    // Create HTTP agents that disable connection pooling
    // This prevents stale connection issues in Docker bridge networks
    this.httpAgent = new http.Agent({
      keepAlive: false,
      maxSockets: Infinity,
      maxFreeSockets: 0,
      timeout: 3000,
      keepAliveMsecs: 0
    });
    
    this.httpsAgent = new https.Agent({
      keepAlive: false,
      maxSockets: Infinity,
      maxFreeSockets: 0,
      timeout: 3000,
      keepAliveMsecs: 0
    });
  }

  /**
   * Get scene and privacy state from controller WebSocket
   * This replaces the old compositor health endpoint
   */
  getSceneState() {
    if (this.controllerWs) {
      return this.controllerWs.getSceneState();
    }
    return {
      currentScene: 'unknown',
      privacyEnabled: false
    };
  }

  /**
   * Get fallback configuration
   */
  async getFallbackConfig() {
    try {
      const data = await fs.readFile(this.fallbackConfigPath, 'utf8');
      return JSON.parse(data);
    } catch (error) {
      // Return default config if file doesn't exist
      return {
        source: 'BLACK',
        imagePath: '/app/shared/offline.png',
        videoPath: '/app/shared/offline.mp4',
        browserUrl: 'https://example.com',
        activeContainer: null
      };
    }
  }

  /**
   * Reconcile fallback config with actual running containers
   * Automatically adjusts the fallback source based on which offline containers are running
   */
  async reconcileFallbackConfig(containers, currentConfig) {
    try {
      // Find running offline containers
      const offlineContainers = {
        'offline-browser': containers.find(c => c.name === 'offline-browser' && c.running),
        'offline-video': containers.find(c => c.name === 'offline-video' && c.running),
        'offline-image': containers.find(c => c.name === 'offline-image' && c.running)
      };

      // Determine which containers are actually running
      const browserRunning = !!offlineContainers['offline-browser'];
      const videoRunning = !!offlineContainers['offline-video'];
      const imageRunning = !!offlineContainers['offline-image'];

      // Determine the correct source based on priority: BROWSER > VIDEO > IMAGE
      let correctSource = 'BLACK';
      let correctActiveContainer = null;

      if (browserRunning) {
        correctSource = 'BROWSER';
        correctActiveContainer = 'offline-browser';
      } else if (videoRunning) {
        correctSource = 'VIDEO';
        correctActiveContainer = 'offline-video';
      } else if (imageRunning) {
        correctSource = 'IMAGE';
        correctActiveContainer = 'offline-image';
      }

      // Check if reconciliation is needed
      const needsUpdate =
        currentConfig.source !== correctSource ||
        currentConfig.activeContainer !== correctActiveContainer;

      if (needsUpdate) {
        // Update config
        const updatedConfig = {
          ...currentConfig,
          source: correctSource,
          activeContainer: correctActiveContainer,
          lastUpdated: new Date().toISOString()
        };

        // Save updated config
        await fs.writeFile(
          this.fallbackConfigPath,
          JSON.stringify(updatedConfig, null, 2)
        );

        console.log(
          `[aggregator] Fallback auto-adjusted: source changed from ${currentConfig.source} to ${correctSource} ` +
          `(activeContainer: ${correctActiveContainer || 'none'})`
        );

        return updatedConfig;
      }

      return currentConfig;
    } catch (error) {
      console.error('[aggregator] Error reconciling fallback config:', error.message);
      return currentConfig; // Return unchanged config on error
    }
  }

  /**
   * Aggregate all data from various sources
   */
  async aggregateData() {
    const timestamp = new Date().toISOString();

    try {
      const [containersResult, systemMetrics, fallbackConfig] = await Promise.all([
        this.controllerService.listContainers(),
        metricsService.getSystemMetrics(),
        this.getFallbackConfig()
      ]);
      
      // Get scene and privacy state from controller WebSocket
      const sceneState = this.getSceneState();
      const currentScene = sceneState.currentScene;
      const privacyEnabled = sceneState.privacyEnabled;
      
      // Extract containers and error state
      let containers = containersResult.containers || [];
      const containersFetchError = containersResult.success === false ? containersResult.error : null;
      
      // Log errors with more context
      if (containersFetchError) {
        console.error('[aggregator] Failed to fetch containers:', containersFetchError);
        console.error('[aggregator] Error code:', containersResult.errorCode);
        console.error('[aggregator] Status code:', containersResult.statusCode);
        console.error('[aggregator] This may indicate that the controller service is unavailable');
        
        // Inject critical containers with 'unknown' status when API fails
        console.log('[aggregator] Injecting critical containers with unknown status');
        containers = CRITICAL_CONTAINERS.map(critical => ({
          name: critical.name,
          full_name: critical.fullName,
          status: 'unknown',
          status_detail: 'Unable to fetch status - controller API unavailable',
          running: false,
          health: 'unknown',
          id: null,
          isCritical: true
        }));
      }

      // Reconcile fallback config with actual running containers
      const reconciledFallbackConfig = await this.reconcileFallbackConfig(containers, fallbackConfig);

      // Check if ffmpeg-kick container is running
      const kickContainer = containers.find(c => c.name === 'ffmpeg-kick');
      const kickStreamingEnabled = kickContainer?.running || false;
      
      // Log if kick streaming status changed
      if (kickStreamingEnabled && !this.lastKickStatus) {
        console.log('[aggregator] Kick streaming detected as STARTED');
      } else if (!kickStreamingEnabled && this.lastKickStatus) {
        console.log('[aggregator] Kick streaming detected as STOPPED');
      }
      this.lastKickStatus = kickStreamingEnabled;

      // Track scene changes and update timestamp
      if (currentScene !== this.streamStatus.currentScene) {
        this.streamStatus.currentScene = currentScene;
        this.streamStatus.stateChangeTimestamp = Date.now();
        console.log(`[aggregator] Scene changed: ${currentScene}`);
      }
      
      // Calculate duration in current scene (seconds)
      const sceneDurationSeconds = Math.floor((Date.now() - this.streamStatus.stateChangeTimestamp) / 1000);

      // Determine if multiplexer is online by checking container status
      const multiplexerContainer = containers.find(c => c.name === 'multiplexer');
      const isOnline = multiplexerContainer?.running && multiplexerContainer?.health === 'healthy';

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
        containersFetchError,
        systemMetrics,
        compositorHealth: {
          status: isOnline ? 'healthy' : 'unavailable',
          current_scene: currentScene,
          srt_connected: currentScene === 'LIVE',  // Infer SRT connection from scene
          srt_bitrate_kbps: 0,  // Not available without compositor
          privacy_enabled: privacyEnabled,
          kick_streaming_enabled: kickStreamingEnabled
        },
        currentScene: currentScene,
        streamStatus: {
          isOnline,
          durationSeconds: sceneDurationSeconds,
          srtConnected: currentScene === 'LIVE',
          srtBitrateKbps: 0,
          privacyEnabled: privacyEnabled
        },
        sceneDurationSeconds,
        cameraConfig: {
          srtUrl: `srt://${this.srtDomain}:${this.srtPort}`
        },
        fallbackConfig: reconciledFallbackConfig
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
          srt_bitrate_kbps: 0,
          privacy_enabled: false,
          kick_streaming_enabled: false
        },
        currentScene: 'unknown',
        streamStatus: {
          isOnline: false,
          durationSeconds,
          srtConnected: false,
          srtBitrateKbps: 0,
          privacyEnabled: false
        },
        sceneDurationSeconds: durationSeconds,
        cameraConfig: {
          srtUrl: `srt://${this.srtDomain}:${this.srtPort}`
        },
        fallbackConfig: {
          source: 'BLACK',
          imagePath: '/app/shared/offline.png',
          videoPath: '/app/shared/offline.mp4',
          browserUrl: 'https://example.com'
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