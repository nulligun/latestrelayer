const axios = require('axios');
const xml2js = require('xml2js');

/**
 * Parses NGINX RTMP statistics from XML endpoint
 */
class RtmpParserService {
  constructor(statsUrl) {
    this.statsUrl = statsUrl;
    this.parser = new xml2js.Parser({ explicitArray: false });
  }

  async getStats() {
    try {
      const response = await axios.get(this.statsUrl, { timeout: 5000 });
      const result = await this.parser.parseStringPromise(response.data);
      
      return this.parseRtmpStats(result);
    } catch (error) {
      console.error('[rtmp] Error fetching RTMP stats:', error.message);
      return {
        inboundBandwidth: 0,
        streams: {}
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
            const totalBw = bwVideo + bwAudio;
            
            // Convert from bytes/sec to kbps
            const bandwidthKbps = Math.round((totalBw * 8) / 1000);

            stats.streams[streamName] = {
              active: stream.active === 'true' || stream.publishing?.active === 'true',
              bandwidth: bandwidthKbps,
              clients: parseInt(stream.nclients || 0),
              publishing: stream.publishing ? true : false
            };

            // Sum up inbound bandwidth (only from publishing streams)
            if (stats.streams[streamName].publishing && 
                (streamName === 'cam' || streamName === 'offline')) {
              stats.inboundBandwidth += bandwidthKbps;
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