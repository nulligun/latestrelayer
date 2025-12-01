const http = require('http');
const https = require('https');

/**
 * Proxy service for controller container management
 * Using native fetch with custom HTTP agent to disable connection pooling
 * This fixes Docker bridge network issues where pooled connections become stale
 */
class ControllerService {
  constructor(controllerUrl) {
    this.controllerUrl = controllerUrl;
    
    // Create HTTP agents that disable connection pooling and keep-alive
    // This prevents stale connection issues in Docker bridge networks
    this.httpAgent = new http.Agent({
      keepAlive: false,
      maxSockets: Infinity,
      maxFreeSockets: 0,
      timeout: 5000,
      keepAliveMsecs: 0
    });
    
    this.httpsAgent = new https.Agent({
      keepAlive: false,
      maxSockets: Infinity,
      maxFreeSockets: 0,
      timeout: 5000,
      keepAliveMsecs: 0
    });
  }

  /**
   * Retry wrapper for fetch requests with exponential backoff
   * Handles transient network issues when Docker reconfigures networks
   */
  async _fetchWithRetry(url, options, maxRetries = 3, timeoutMs = 5000) {
    const baseDelay = 1000; // 1 second base delay
    
    for (let attempt = 1; attempt <= maxRetries; attempt++) {
      const controller = new AbortController();
      const timeoutId = setTimeout(() => controller.abort(), timeoutMs);
      
      try {
        // Add custom agent and Connection: close header to prevent connection pooling
        const fetchOptions = {
          ...options,
          signal: controller.signal,
          headers: {
            ...options.headers,
            'Connection': 'close'  // Force new connection for each request
          },
          // Use custom agent based on protocol
          agent: (parsedUrl) => {
            return parsedUrl.protocol === 'http:' ? this.httpAgent : this.httpsAgent;
          }
        };
        
        const response = await fetch(url, fetchOptions);
        clearTimeout(timeoutId);
        return { success: true, response };
        
      } catch (error) {
        clearTimeout(timeoutId);
        
        const isTimeout = error.name === 'AbortError';
        const isNetworkError = error.message?.includes('fetch failed') ||
                               error.code === 'ECONNREFUSED' ||
                               error.message?.includes('ENOTFOUND');
        
        // Only retry on timeout or network errors
        if (attempt < maxRetries && (isTimeout || isNetworkError)) {
          const delay = baseDelay * Math.pow(2, attempt - 1); // Exponential backoff
          console.log(`[controller] Retry ${attempt}/${maxRetries} after ${delay}ms (${error.name}: ${error.message})`);
          await new Promise(resolve => setTimeout(resolve, delay));
          continue;
        }
        
        // Final attempt failed or non-retryable error
        return { success: false, error };
      }
    }
  }

  async listContainers() {
    const startTime = Date.now();
    const url = `${this.controllerUrl}/containers`;
    
    console.log(`[controller] ========================================`);
    console.log(`[controller] Starting listContainers() request`);
    console.log(`[controller] URL: ${url}`);
    console.log(`[controller] Timestamp: ${new Date().toISOString()}`);
    
    try {
      console.log(`[controller] Initiating fetch request with retry logic...`);
      const result = await this._fetchWithRetry(url, {
        method: 'GET',
        headers: {
          'Accept': 'application/json'
        }
      });
      
      if (!result.success) {
        const duration = Date.now() - startTime;
        const error = result.error;
        
        console.error(`[controller] ========================================`);
        console.error(`[controller] All retry attempts FAILED after ${duration}ms`);
        console.error(`[controller] Error type: ${error.name}`);
        console.error(`[controller] Error message: ${error.message}`);
        
        let errorMessage = error.message;
        let errorType = 'unknown';
        
        if (error.name === 'AbortError') {
          errorType = 'timeout';
          errorMessage = `Request timeout after ${duration}ms - Controller may be overloaded or unreachable`;
        } else if (error.message.includes('fetch failed') || error.code === 'ECONNREFUSED') {
          errorType = 'connection';
          errorMessage = `Cannot connect to controller at ${url} - service may be down`;
        } else if (error.message.includes('ENOTFOUND')) {
          errorType = 'dns';
          errorMessage = `DNS resolution failed for controller URL: ${url}`;
        }
        
        console.error(`[controller] ========================================`);
        
        return {
          success: false,
          containers: [],
          error: errorMessage,
          errorType: errorType,
          errorCode: error.code,
          duration: duration
        };
      }
      
      const response = result.response;
      const duration = Date.now() - startTime;
      
      console.log(`[controller] Response received in ${duration}ms`);
      console.log(`[controller] Response status: ${response.status} ${response.statusText}`);
      console.log(`[controller] Response headers:`, JSON.stringify(Object.fromEntries(response.headers.entries())));
      
      // Get raw response text BEFORE parsing
      const rawText = await response.text();
      console.log(`[controller] Raw response text (${rawText.length} chars):`, rawText);
      
      // Check if response is OK
      if (!response.ok) {
        console.error(`[controller] HTTP error! status: ${response.status}`);
        return {
          success: false,
          containers: [],
          error: `HTTP ${response.status}: ${response.statusText}`,
          statusCode: response.status,
          duration: duration,
          rawResponse: rawText
        };
      }
      
      // Now try to parse JSON
      let data;
      try {
        console.log(`[controller] Attempting to parse JSON...`);
        data = JSON.parse(rawText);
        console.log(`[controller] JSON parsed successfully`);
        console.log(`[controller] Parsed data keys:`, Object.keys(data));
      } catch (parseError) {
        console.error(`[controller] JSON parsing failed!`);
        console.error(`[controller] Parse error:`, parseError.message);
        console.error(`[controller] Raw text that failed to parse:`, rawText);
        return {
          success: false,
          containers: [],
          error: `JSON parsing failed: ${parseError.message}`,
          parseError: parseError.message,
          duration: duration,
          rawResponse: rawText
        };
      }
      
      console.log(`[controller] Request completed successfully in ${duration}ms`);
      console.log(`[controller] Container count: ${data.containers?.length || 0}`);
      console.log(`[controller] ========================================`);
      
      return {
        success: true,
        containers: data.containers || [],
        error: null
      };
      
    } catch (error) {
      const duration = Date.now() - startTime;
      console.error(`[controller] Unexpected error during listContainers: ${error.message}`);
      console.error(`[controller] ========================================`);
      
      return {
        success: false,
        containers: [],
        error: error.message,
        errorType: 'unexpected',
        duration: duration
      };
    }
  }

  async getContainerStatus(containerName) {
    const url = `${this.controllerUrl}/container/${containerName}/status`;
    
    try {
      console.log(`[controller] Getting status for ${containerName}`);
      const result = await this._fetchWithRetry(url, {
        method: 'GET',
        headers: { 'Accept': 'application/json' }
      });
      
      if (!result.success) {
        console.error(`[controller] Failed to get status for ${containerName}:`, result.error.message);
        return { error: result.error.message };
      }
      
      const response = result.response;
      const rawText = await response.text();
      console.log(`[controller] Status response (${response.status}):`, rawText);
      
      if (!response.ok) {
        return { error: `HTTP ${response.status}: ${response.statusText}` };
      }
      
      return JSON.parse(rawText);
    } catch (error) {
      console.error(`[controller] Error getting status for ${containerName}:`, error.message);
      return { error: error.message };
    }
  }

  async startContainer(containerName) {
    const url = `${this.controllerUrl}/container/${containerName}/start`;
    
    try {
      console.log(`[controller] Starting container: ${containerName}`);
      const result = await this._fetchWithRetry(url, {
        method: 'POST',
        headers: { 'Accept': 'application/json', 'Content-Type': 'application/json' },
        body: '{}'
      }, 3, 30000); // 30s timeout for container operations
      
      if (!result.success) {
        console.error(`[controller] Failed to start ${containerName}:`, result.error.message);
        return { error: result.error.message };
      }
      
      const response = result.response;
      const rawText = await response.text();
      console.log(`[controller] Start response (${response.status}):`, rawText);
      
      if (!response.ok) {
        return { error: `HTTP ${response.status}: ${response.statusText}` };
      }
      
      return JSON.parse(rawText);
    } catch (error) {
      console.error(`[controller] Error starting ${containerName}:`, error.message);
      return { error: error.message };
    }
  }

  async stopContainer(containerName) {
    const url = `${this.controllerUrl}/container/${containerName}/stop`;
    
    try {
      console.log(`[controller] Stopping container: ${containerName}`);
      const result = await this._fetchWithRetry(url, {
        method: 'POST',
        headers: { 'Accept': 'application/json', 'Content-Type': 'application/json' },
        body: '{}'
      }, 3, 30000); // 30s timeout for container operations
      
      if (!result.success) {
        console.error(`[controller] Failed to stop ${containerName}:`, result.error.message);
        return { error: result.error.message };
      }
      
      const response = result.response;
      const rawText = await response.text();
      console.log(`[controller] Stop response (${response.status}):`, rawText);
      
      if (!response.ok) {
        return { error: `HTTP ${response.status}: ${response.statusText}` };
      }
      
      return JSON.parse(rawText);
    } catch (error) {
      console.error(`[controller] Error stopping ${containerName}:`, error.message);
      return { error: error.message };
    }
  }

  async restartContainer(containerName) {
    const url = `${this.controllerUrl}/container/${containerName}/restart`;
    
    try {
      console.log(`[controller] Restarting container: ${containerName}`);
      const result = await this._fetchWithRetry(url, {
        method: 'POST',
        headers: { 'Accept': 'application/json', 'Content-Type': 'application/json' },
        body: '{}'
      }, 3, 30000); // 30s timeout for container operations
      
      if (!result.success) {
        console.error(`[controller] Failed to restart ${containerName}:`, result.error.message);
        return { error: result.error.message };
      }
      
      const response = result.response;
      const rawText = await response.text();
      console.log(`[controller] Restart response (${response.status}):`, rawText);
      
      if (!response.ok) {
        return { error: `HTTP ${response.status}: ${response.statusText}` };
      }
      
      return JSON.parse(rawText);
    } catch (error) {
      console.error(`[controller] Error restarting ${containerName}:`, error.message);
      return { error: error.message };
    }
  }

  async createAndStartContainer(containerName) {
    const url = `${this.controllerUrl}/container/${containerName}/create-and-start`;
    
    try {
      console.log(`[controller] Creating and starting container: ${containerName}`);
      const result = await this._fetchWithRetry(url, {
        method: 'POST',
        headers: { 'Accept': 'application/json', 'Content-Type': 'application/json' },
        body: '{}'
      }, 3, 30000); // 30s timeout for container operations
      
      if (!result.success) {
        console.error(`[controller] Failed to create and start ${containerName}:`, result.error.message);
        return { error: result.error.message };
      }
      
      const response = result.response;
      const rawText = await response.text();
      console.log(`[controller] Create-and-start response (${response.status}):`, rawText);
      
      if (!response.ok) {
        return { error: `HTTP ${response.status}: ${response.statusText}` };
      }
      
      return JSON.parse(rawText);
    } catch (error) {
      console.error(`[controller] Error creating and starting ${containerName}:`, error.message);
      return { error: error.message };
    }
  }

  async getContainerLogs(containerName, tail = 500) {
    const url = `${this.controllerUrl}/container/${containerName}/logs?tail=${tail}`;
    
    try {
      console.log(`[controller] Getting logs for ${containerName} (tail=${tail})`);
      const result = await this._fetchWithRetry(url, {
        method: 'GET',
        headers: { 'Accept': 'application/json' }
      }, 3, 10000); // 10s timeout for logs
      
      if (!result.success) {
        console.error(`[controller] Failed to get logs for ${containerName}:`, result.error.message);
        return { error: result.error.message, logs: [] };
      }
      
      const response = result.response;
      const rawText = await response.text();
      
      if (!response.ok) {
        console.error(`[controller] Logs response error (${response.status}):`, rawText);
        return { error: `HTTP ${response.status}: ${response.statusText}`, logs: [] };
      }
      
      return JSON.parse(rawText);
    } catch (error) {
      console.error(`[controller] Error getting logs for ${containerName}:`, error.message);
      return { error: error.message, logs: [] };
    }
  }

  // Privacy mode endpoints
  
  async enablePrivacyMode() {
    const url = `${this.controllerUrl}/privacy/enable`;
    
    try {
      console.log(`[controller] Enabling privacy mode`);
      const result = await this._fetchWithRetry(url, {
        method: 'POST',
        headers: { 'Accept': 'application/json', 'Content-Type': 'application/json' },
        body: '{}'
      }, 3, 10000);
      
      if (!result.success) {
        console.error(`[controller] Failed to enable privacy mode:`, result.error.message);
        return { error: result.error.message };
      }
      
      const response = result.response;
      const rawText = await response.text();
      console.log(`[controller] Enable privacy response (${response.status}):`, rawText);
      
      if (!response.ok) {
        return { error: `HTTP ${response.status}: ${response.statusText}` };
      }
      
      return JSON.parse(rawText);
    } catch (error) {
      console.error(`[controller] Error enabling privacy mode:`, error.message);
      return { error: error.message };
    }
  }

  async disablePrivacyMode() {
    const url = `${this.controllerUrl}/privacy/disable`;
    
    try {
      console.log(`[controller] Disabling privacy mode`);
      const result = await this._fetchWithRetry(url, {
        method: 'POST',
        headers: { 'Accept': 'application/json', 'Content-Type': 'application/json' },
        body: '{}'
      }, 3, 10000);
      
      if (!result.success) {
        console.error(`[controller] Failed to disable privacy mode:`, result.error.message);
        return { error: result.error.message };
      }
      
      const response = result.response;
      const rawText = await response.text();
      console.log(`[controller] Disable privacy response (${response.status}):`, rawText);
      
      if (!response.ok) {
        return { error: `HTTP ${response.status}: ${response.statusText}` };
      }
      
      return JSON.parse(rawText);
    } catch (error) {
      console.error(`[controller] Error disabling privacy mode:`, error.message);
      return { error: error.message };
    }
  }

  async getPrivacyMode() {
    const url = `${this.controllerUrl}/privacy`;
    
    try {
      console.log(`[controller] Getting privacy mode`);
      const result = await this._fetchWithRetry(url, {
        method: 'GET',
        headers: { 'Accept': 'application/json' }
      }, 3, 5000);
      
      if (!result.success) {
        console.error(`[controller] Failed to get privacy mode:`, result.error.message);
        return { error: result.error.message, privacy_enabled: false };
      }
      
      const response = result.response;
      const rawText = await response.text();
      
      if (!response.ok) {
        console.error(`[controller] Privacy response error (${response.status}):`, rawText);
        return { error: `HTTP ${response.status}: ${response.statusText}`, privacy_enabled: false };
      }
      
      return JSON.parse(rawText);
    } catch (error) {
      console.error(`[controller] Error getting privacy mode:`, error.message);
      return { error: error.message, privacy_enabled: false };
    }
  }

  // Scene endpoints
  
  async getCurrentScene() {
    const url = `${this.controllerUrl}/scene`;
    
    try {
      console.log(`[controller] Getting current scene`);
      const result = await this._fetchWithRetry(url, {
        method: 'GET',
        headers: { 'Accept': 'application/json' }
      }, 3, 5000);
      
      if (!result.success) {
        console.error(`[controller] Failed to get current scene:`, result.error.message);
        return { error: result.error.message, scene: 'unknown' };
      }
      
      const response = result.response;
      const rawText = await response.text();
      
      if (!response.ok) {
        console.error(`[controller] Scene response error (${response.status}):`, rawText);
        return { error: `HTTP ${response.status}: ${response.statusText}`, scene: 'unknown' };
      }
      
      return JSON.parse(rawText);
    } catch (error) {
      console.error(`[controller] Error getting current scene:`, error.message);
      return { error: error.message, scene: 'unknown' };
    }
  }
}

module.exports = ControllerService;