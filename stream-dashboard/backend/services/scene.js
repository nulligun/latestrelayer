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

  async getSceneMode() {
    try {
      const response = await axios.get(
        `${this.muxerUrl}/scene/mode`,
        { timeout: 5000 }
      );
      return response.data;
    } catch (error) {
      console.error('[scene] Error getting scene mode:', error.message);
      throw new Error(`Failed to get scene mode: ${error.message}`);
    }
  }

  async setCameraMode() {
    try {
      const response = await axios.post(
        `${this.muxerUrl}/scene/camera`,
        {},
        { timeout: 5000 }
      );
      return response.data;
    } catch (error) {
      console.error('[scene] Error setting camera mode:', error.message);
      throw new Error(`Failed to set camera mode: ${error.message}`);
    }
  }

  async setPrivacyMode() {
    try {
      const response = await axios.post(
        `${this.muxerUrl}/scene/privacy`,
        {},
        { timeout: 5000 }
      );
      return response.data;
    } catch (error) {
      console.error('[scene] Error setting privacy mode:', error.message);
      throw new Error(`Failed to set privacy mode: ${error.message}`);
    }
  }
}

module.exports = SceneService;