const WebSocket = require('ws');
const EventEmitter = require('events');

/**
 * WebSocket client for connecting to the controller
 * Handles reconnection, message parsing, and event emission
 */
class ControllerWebSocketClient extends EventEmitter {
  constructor(controllerUrl) {
    super();
    
    // Convert HTTP URL to WebSocket URL (ws://controller:8090)
    const url = new URL(controllerUrl);
    this.wsUrl = `ws://${url.hostname}:8090`;
    
    this.ws = null;
    this.reconnectAttempts = 0;
    this.maxReconnectAttempts = Infinity;
    this.reconnectDelay = 2000; // Start with 2 seconds
    this.maxReconnectDelay = 30000; // Max 30 seconds
    this.isConnected = false;
    this.shouldReconnect = true;
    
    // Queue for pending log subscriptions when not connected
    this.pendingSubscriptions = new Map(); // containerName -> { lines }
    
    // Scene and privacy state from controller
    this.currentScene = 'unknown';
    this.privacyEnabled = false;
    
    console.log(`[controller-ws] Initialized with URL: ${this.wsUrl}`);
  }
  
  connect() {
    if (this.ws && (this.ws.readyState === WebSocket.CONNECTING || this.ws.readyState === WebSocket.OPEN)) {
      console.log('[controller-ws][startup-debug] Already connected or connecting');
      return;
    }
    
    console.log(`[controller-ws][startup-debug] Connecting to ${this.wsUrl}...`);
    
    try {
      this.ws = new WebSocket(this.wsUrl);
      console.log(`[controller-ws][startup-debug] WebSocket object created, waiting for 'open' event...`);
      
      this.ws.on('open', () => {
        console.log('[controller-ws][startup-debug] Connected SUCCESSFULLY to controller');
        this.isConnected = true;
        this.reconnectAttempts = 0;
        this.reconnectDelay = 2000;
        
        // Process any pending log subscriptions
        this.processPendingSubscriptions();
        
        console.log('[controller-ws][startup-debug] Emitting "connected" event');
        this.emit('connected');
      });
      
      this.ws.on('message', (data) => {
        try {
          const message = JSON.parse(data.toString());
          console.log(`[scene_change_debug] Received WebSocket message type: ${message.type}`);
          this.handleMessage(message);
        } catch (error) {
          console.error('[controller-ws] Error parsing message:', error.message);
        }
      });
      
      this.ws.on('error', (error) => {
        console.error('[controller-ws][startup-debug] WebSocket ERROR:', error.message);
        this.emit('error', error);
      });
      
      this.ws.on('close', (code, reason) => {
        console.log(`[controller-ws][startup-debug] Connection CLOSED (code: ${code}, reason: ${reason || 'none'})`);
        this.isConnected = false;
        this.emit('disconnected');
        
        if (this.shouldReconnect) {
          this.scheduleReconnect();
        }
      });
      
    } catch (error) {
      console.error('[controller-ws] Error creating WebSocket:', error.message);
      if (this.shouldReconnect) {
        this.scheduleReconnect();
      }
    }
  }
  
  scheduleReconnect() {
    this.reconnectAttempts++;
    
    if (this.reconnectAttempts > this.maxReconnectAttempts) {
      console.error('[controller-ws] Max reconnection attempts reached');
      return;
    }
    
    console.log(`[controller-ws] Reconnecting in ${this.reconnectDelay / 1000}s (attempt ${this.reconnectAttempts})...`);
    
    setTimeout(() => {
      this.connect();
    }, this.reconnectDelay);
    
    // Exponential backoff with max cap
    this.reconnectDelay = Math.min(this.reconnectDelay * 2, this.maxReconnectDelay);
  }
  
  handleMessage(message) {
    const { type } = message;
    
    switch (type) {
      case 'initial_state':
        console.log(`[controller-ws][startup-debug] Received INITIAL_STATE with ${message.containers?.length || 0} containers`);
        console.log(`[controller-ws][startup-debug] Scene from initial_state: ${message.current_scene}`);
        // Update scene and privacy state from initial state
        if (message.current_scene !== undefined) {
          this.currentScene = message.current_scene;
          console.log(`[controller-ws][startup-debug] Set currentScene to: ${this.currentScene}`);
        }
        if (message.privacy_enabled !== undefined) {
          this.privacyEnabled = message.privacy_enabled;
          console.log(`[controller-ws][startup-debug] Set privacyEnabled to: ${this.privacyEnabled}`);
        }
        console.log(`[controller-ws][startup-debug] Emitting 'initial_state' event to server.js`);
        this.emit('initial_state', message);
        break;
        
      case 'status_change':
        console.log(`[controller-ws] Container status changed: ${message.changes?.length || 0} change(s)`);
        // Update scene and privacy state from status change
        if (message.current_scene !== undefined) {
          const previousScene = this.currentScene;
          this.currentScene = message.current_scene;
          if (previousScene !== this.currentScene) {
            console.log(`[controller-ws] Scene changed: ${previousScene} -> ${this.currentScene}`);
            this.emit('scene_change', {
              previousScene,
              currentScene: this.currentScene
            });
          }
        }
        if (message.privacy_enabled !== undefined) {
          const previousPrivacy = this.privacyEnabled;
          this.privacyEnabled = message.privacy_enabled;
          if (previousPrivacy !== this.privacyEnabled) {
            console.log(`[controller-ws] Privacy mode changed: ${this.privacyEnabled}`);
            this.emit('privacy_change', {
              privacyEnabled: this.privacyEnabled
            });
          }
        }
        this.emit('status_change', message);
        break;
        
      case 'scene_change':
        console.log(`[controller-ws] Scene change received: ${message.current_scene}`);
        console.log(`[scene_change_debug] controllerWebSocket received scene_change: ${JSON.stringify(message)}`);
        this.currentScene = message.current_scene;
        if (message.privacy_enabled !== undefined) {
          this.privacyEnabled = message.privacy_enabled;
        }
        console.log(`[scene_change_debug] Emitting scene_change event with currentScene=${this.currentScene}`);
        this.emit('scene_change', {
          currentScene: this.currentScene,
          privacyEnabled: this.privacyEnabled,
          changeData: message.change_data,
          timestamp: message.timestamp
        });
        break;
        
      case 'privacy_change':
        console.log(`[controller-ws] Privacy change received: ${message.privacy_enabled}`);
        console.log(`[scene_change_debug] controllerWebSocket received privacy_change: ${JSON.stringify(message)}`);
        this.privacyEnabled = message.privacy_enabled;
        if (message.current_scene !== undefined) {
          this.currentScene = message.current_scene;
        }
        console.log(`[scene_change_debug] Emitting privacy_change event with privacyEnabled=${this.privacyEnabled}`);
        this.emit('privacy_change', {
          privacyEnabled: this.privacyEnabled,
          currentScene: this.currentScene,
          changeData: message.change_data,
          timestamp: message.timestamp
        });
        break;
        
      case 'new_logs':
        this.emit('new_logs', message);
        break;
        
      case 'log_snapshot':
        console.log(`[controller-ws] Log snapshot received for ${message.container}: ${message.logs?.length || 0} lines`);
        this.emit('log_snapshot', message);
        break;
        
      default:
        console.warn(`[controller-ws] Unknown message type: ${type}`);
    }
  }
  
  subscribeToLogs(containerName, lines = 100) {
    if (!this.isConnected) {
      // Queue the subscription for when we connect
      console.log(`[controller-ws] Queueing log subscription for ${containerName} (not connected yet)`);
      this.pendingSubscriptions.set(containerName, { lines });
      return;
    }
    
    const message = {
      type: 'subscribe_logs',
      container: containerName,
      lines: lines
    };
    
    try {
      this.ws.send(JSON.stringify(message));
      console.log(`[controller-ws] Subscribed to logs for ${containerName}`);
    } catch (error) {
      console.error(`[controller-ws] Error subscribing to logs:`, error.message);
    }
  }
  
  processPendingSubscriptions() {
    if (this.pendingSubscriptions.size === 0) {
      return;
    }
    
    console.log(`[controller-ws] Processing ${this.pendingSubscriptions.size} pending log subscription(s)`);
    
    for (const [containerName, { lines }] of this.pendingSubscriptions) {
      const message = {
        type: 'subscribe_logs',
        container: containerName,
        lines: lines
      };
      
      try {
        this.ws.send(JSON.stringify(message));
        console.log(`[controller-ws] Processed pending subscription for ${containerName}`);
      } catch (error) {
        console.error(`[controller-ws] Error processing pending subscription for ${containerName}:`, error.message);
      }
    }
    
    // Clear the queue after processing
    this.pendingSubscriptions.clear();
  }
  
  unsubscribeFromLogs(containerName) {
    if (!this.isConnected) {
      return;
    }
    
    const message = {
      type: 'unsubscribe_logs',
      container: containerName
    };
    
    try {
      this.ws.send(JSON.stringify(message));
      console.log(`[controller-ws] Unsubscribed from logs for ${containerName}`);
    } catch (error) {
      console.error(`[controller-ws] Error unsubscribing from logs:`, error.message);
    }
  }
  
  requestStateUpdate() {
    if (!this.isConnected) {
      console.log('[controller-ws] Not connected, cannot request state update');
      return;
    }
    
    const message = {
      type: 'request_state_update'
    };
    
    try {
      this.ws.send(JSON.stringify(message));
      console.log('[controller-ws] Requested state update from controller (will query multiplexer)');
    } catch (error) {
      console.error('[controller-ws] Error requesting state update:', error.message);
    }
  }
  
  disconnect() {
    console.log('[controller-ws] Disconnecting...');
    this.shouldReconnect = false;
    
    if (this.ws) {
      this.ws.close();
      this.ws = null;
    }
    
    this.isConnected = false;
  }
  
  getConnectionStatus() {
    return {
      connected: this.isConnected,
      reconnectAttempts: this.reconnectAttempts,
      wsUrl: this.wsUrl
    };
  }
  
  /**
   * Get the current scene state
   */
  getCurrentScene() {
    return this.currentScene;
  }
  
  /**
   * Get the current privacy mode state
   */
  getPrivacyEnabled() {
    return this.privacyEnabled;
  }
  
  /**
   * Get both scene and privacy state
   */
  getSceneState() {
    return {
      currentScene: this.currentScene,
      privacyEnabled: this.privacyEnabled
    };
  }
}

module.exports = ControllerWebSocketClient;