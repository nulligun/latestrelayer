/**
 * Proxy service for compositor scene and privacy control
 */
class SceneService {
  constructor(compositorUrl) {
    this.compositorUrl = compositorUrl;
  }

  async getCurrentScene() {
    const url = `${this.compositorUrl}/health`;
    const controller = new AbortController();
    const timeoutId = setTimeout(() => controller.abort(), 5000);
    
    try {
      const response = await fetch(url, {
        method: 'GET',
        signal: controller.signal,
        headers: { 'Accept': 'application/json' }
      });
      clearTimeout(timeoutId);
      
      if (!response.ok) {
        throw new Error(`HTTP ${response.status}: ${response.statusText}`);
      }
      
      const data = await response.json();
      return { scene: data.scene };
    } catch (error) {
      clearTimeout(timeoutId);
      console.error('[scene] Error getting current scene:', error.message);
      throw new Error(`Failed to get current scene: ${error.message}`);
    }
  }

  async setPrivacyMode(enabled) {
    const url = `${this.compositorUrl}/privacy`;
    const controller = new AbortController();
    const timeoutId = setTimeout(() => controller.abort(), 5000);
    
    try {
      const response = await fetch(url, {
        method: 'POST',
        signal: controller.signal,
        headers: {
          'Accept': 'application/json',
          'Content-Type': 'application/json'
        },
        body: JSON.stringify({ enabled })
      });
      clearTimeout(timeoutId);
      
      if (!response.ok) {
        throw new Error(`HTTP ${response.status}: ${response.statusText}`);
      }
      
      return await response.json();
    } catch (error) {
      clearTimeout(timeoutId);
      console.error('[scene] Error setting privacy mode:', error.message);
      throw new Error(`Failed to set privacy mode: ${error.message}`);
    }
  }

  async getPrivacyMode() {
    const url = `${this.compositorUrl}/privacy`;
    const controller = new AbortController();
    const timeoutId = setTimeout(() => controller.abort(), 5000);
    
    try {
      const response = await fetch(url, {
        method: 'GET',
        signal: controller.signal,
        headers: { 'Accept': 'application/json' }
      });
      clearTimeout(timeoutId);
      
      if (!response.ok) {
        throw new Error(`HTTP ${response.status}: ${response.statusText}`);
      }
      
      return await response.json();
    } catch (error) {
      clearTimeout(timeoutId);
      console.error('[scene] Error getting privacy mode:', error.message);
      throw new Error(`Failed to get privacy mode: ${error.message}`);
    }
  }
}

module.exports = SceneService;