const axios = require('axios');

/**
 * Proxy service for muxer scene switching
 */
class SceneService {
  constructor(muxerUrl) {
    this.muxerUrl = muxerUrl;
  }

  async switchScene(sceneName) {
    try {
      const response = await axios.get(
        `${this.muxerUrl}/switch?src=${sceneName}`,
        { timeout: 5000 }
      );
      return { success: true, scene: sceneName, message: response.data };
    } catch (error) {
      console.error(`[scene] Error switching to ${sceneName}:`, error.message);
      throw new Error(`Failed to switch scene: ${error.message}`);
    }
  }

  async getCurrentScene() {
    try {
      const response = await axios.get(
        `${this.muxerUrl}/scene`,
        { timeout: 5000 }
      );
      return response.data;
    } catch (error) {
      console.error('[scene] Error getting current scene:', error.message);
      throw new Error(`Failed to get current scene: ${error.message}`);
    }
  }
}

module.exports = SceneService;