const axios = require('axios');

/**
 * Proxy service for compositor scene and privacy control
 */
class SceneService {
  constructor(compositorUrl) {
    this.compositorUrl = compositorUrl;
  }

  async getCurrentScene() {
    try {
      const response = await axios.get(
        `${this.compositorUrl}/scene`,
        { timeout: 5000 }
      );
      return response.data;
    } catch (error) {
      console.error('[scene] Error getting current scene:', error.message);
      throw new Error(`Failed to get current scene: ${error.message}`);
    }
  }

  async setPrivacyMode(enabled) {
    try {
      const response = await axios.post(
        `${this.compositorUrl}/privacy`,
        { enabled },
        { timeout: 5000 }
      );
      return response.data;
    } catch (error) {
      console.error('[scene] Error setting privacy mode:', error.message);
      throw new Error(`Failed to set privacy mode: ${error.message}`);
    }
  }

  async getPrivacyMode() {
    try {
      const response = await axios.get(
        `${this.compositorUrl}/privacy`,
        { timeout: 5000 }
      );
      return response.data;
    } catch (error) {
      console.error('[scene] Error getting privacy mode:', error.message);
      throw new Error(`Failed to get privacy mode: ${error.message}`);
    }
  }
}

module.exports = SceneService;