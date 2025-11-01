const axios = require('axios');
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
      const response = await axios.get(this.statsUrl, { timeout: 5000 });
      const result = await this.parser.parseStringPromise(response.data);
      
      const stats = this.parseRtmpStats(result);
      
      // Fetch current scene from stream-switcher
      if (this.switcherUrl) {
        try {
          const sceneResponse = await axios.get(`${this.switcherUrl}/scene`, { timeout: 2000 });
          stats.currentScene = sceneResponse.data.scene;
        } catch (sceneError) {
          console.error('[rtmp] Error fetching current scene:', sceneError.message);
          stats.currentScene = null;
        }
      }
      
      return stats;
    } catch (error) {
      console.error('[rtmp] Error fetching RTMP stats:', error.message);
      return {
        inboundBandwidth: 0,
        streams: {},
        currentScene: null
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