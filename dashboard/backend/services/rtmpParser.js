const xml2js = require('xml2js');

/**
 * Parses NGINX RTMP statistics from XML endpoint
 */
class RtmpParserService {
  constructor(statsUrl, switcherUrl) {
    this.statsUrl = statsUrl;
    this.switcherUrl = switcherUrl;
    this.parser = new xml2js.Parser({ explicitArray: false });
  }

  async getStats() {
    try {
      // Fetch RTMP stats XML
      const controller = new AbortController();
      const timeoutId = setTimeout(() => controller.abort(), 5000);
      
      const response = await fetch(this.statsUrl, {
        method: 'GET',
        signal: controller.signal
      });
      clearTimeout(timeoutId);
      
      if (!response.ok) {
        throw new Error(`HTTP ${response.status}: ${response.statusText}`);
      }
      
      const xmlText = await response.text();
      const result = await this.parser.parseStringPromise(xmlText);
      
      const stats = this.parseRtmpStats(result);
      
      // Fetch enhanced health data from muxer (includes source availability)
      if (this.switcherUrl) {
        try {
          const healthController = new AbortController();
          const healthTimeoutId = setTimeout(() => healthController.abort(), 2000);
          
          const healthResponse = await fetch(`${this.switcherUrl}/health`, {
            method: 'GET',
            signal: healthController.signal,
            headers: { 'Accept': 'application/json' }
          });
          clearTimeout(healthTimeoutId);
          
          if (healthResponse.ok) {
            const healthData = await healthResponse.json();
            
            // Check if response has sources (new format)
            if (healthData.sources) {
              // New enhanced health endpoint
              stats.currentScene = healthData.current_scene;
              stats.sourceAvailability = healthData.sources;
              stats.muxerStatus = {
                pipeline_state: healthData.pipeline_state,
                uptime_seconds: healthData.uptime_seconds
              };
            } else {
              // Fallback to old /scene endpoint for backward compatibility
              try {
                const sceneController = new AbortController();
                const sceneTimeoutId = setTimeout(() => sceneController.abort(), 2000);
                
                const sceneResponse = await fetch(`${this.switcherUrl}/scene`, {
                  method: 'GET',
                  signal: sceneController.signal,
                  headers: { 'Accept': 'application/json' }
                });
                clearTimeout(sceneTimeoutId);
                
                if (sceneResponse.ok) {
                  const sceneData = await sceneResponse.json();
                  stats.currentScene = sceneData.scene;
                }
              } catch (sceneError) {
                console.error('[rtmp] Error fetching current scene:', sceneError.message);
                stats.currentScene = null;
              }
            }
          }
        } catch (healthError) {
          console.error('[rtmp] Error fetching muxer health:', healthError.message);
          stats.currentScene = null;
          stats.sourceAvailability = null;
        }
      }
      
      return stats;
    } catch (error) {
      console.error('[rtmp] Error fetching RTMP stats:', error.message);
      return {
        inboundBandwidth: 0,
        streams: {},
        currentScene: null,
        sourceAvailability: null
      };
    }
  }

  parseRtmpStats(xmlData) {
    const stats = {
      inboundBandwidth: 0,
      streams: {}
    };

    try {
      if (!xmlData.rtmp || !xmlData.rtmp.server || !xmlData.rtmp.server.application) {
        return stats;
      }

      const applications = Array.isArray(xmlData.rtmp.server.application)
        ? xmlData.rtmp.server.application
        : [xmlData.rtmp.server.application];

      for (const app of applications) {
        if (app.name === 'live' && app.live && app.live.stream) {
          const streams = Array.isArray(app.live.stream)
            ? app.live.stream
            : [app.live.stream];

          for (const stream of streams) {
            const streamName = stream.name;
            const bwVideo = parseInt(stream.bw_video || 0);
            const bwAudio = parseInt(stream.bw_audio || 0);
            const bwOut = parseInt(stream.bw_out || 0);
            const totalBw = bwVideo + bwAudio;
            
            // Convert from bps to Kbps
            const bandwidthKbps = Math.round(totalBw / 1024);

            stats.streams[streamName] = {
              active: bwOut > 0,
              bandwidth: bandwidthKbps,
              clients: parseInt(stream.nclients || 0),
              publishing: stream.hasOwnProperty('publishing')
            };

            // Camera Bitrate is only from the cam stream
            if (streamName === 'cam') {
              stats.inboundBandwidth = bandwidthKbps;
            }
          }
        }
      }
    } catch (error) {
      console.error('[rtmp] Error parsing RTMP stats:', error.message);
    }

    return stats;
  }
}

module.exports = RtmpParserService;