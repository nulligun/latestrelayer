const axios = require('axios');

/**
 * Proxy service for stream-controller container management
 */
class ControllerService {
  constructor(controllerUrl) {
    this.controllerUrl = controllerUrl;
  }

  async listContainers() {
    try {
      const response = await axios.get(`${this.controllerUrl}/containers`, { timeout: 5000 });
      return response.data.containers || [];
    } catch (error) {
      console.error('[controller] Error listing containers:', error.message);
      return [];
    }
  }

  async getContainerStatus(containerName) {
    try {
      const response = await axios.get(
        `${this.controllerUrl}/container/${containerName}/status`,
        { timeout: 5000 }
      );
      return response.data;
    } catch (error) {
      console.error(`[controller] Error getting status for ${containerName}:`, error.message);
      return { error: error.message };
    }
  }

  async startContainer(containerName) {
    try {
      const response = await axios.post(
        `${this.controllerUrl}/container/${containerName}/start`,
        {},
        { timeout: 30000 }
      );
      return response.data;
    } catch (error) {
      console.error(`[controller] Error starting ${containerName}:`, error.message);
      return { error: error.message };
    }
  }

  async stopContainer(containerName) {
    try {
      const response = await axios.post(
        `${this.controllerUrl}/container/${containerName}/stop`,
        {},
        { timeout: 30000 }
      );
      return response.data;
    } catch (error) {
      console.error(`[controller] Error stopping ${containerName}:`, error.message);
      return { error: error.message };
    }
  }

  async restartContainer(containerName) {
    try {
      const response = await axios.post(
        `${this.controllerUrl}/container/${containerName}/restart`,
        {},
        { timeout: 30000 }
      );
      return response.data;
    } catch (error) {
      console.error(`[controller] Error restarting ${containerName}:`, error.message);
      return { error: error.message };
    }
  }

  async createAndStartContainer(containerName) {
    try {
      const response = await axios.post(
        `${this.controllerUrl}/container/${containerName}/create-and-start`,
        {},
        { timeout: 30000 }
      );
      return response.data;
    } catch (error) {
      console.error(`[controller] Error creating and starting ${containerName}:`, error.message);
      return { error: error.message };
    }
  }
}

module.exports = ControllerService;