export class WebSocketService {
  constructor() {
    this.ws = null;
    this.listeners = [];
    this.reconnectAttempts = 0;
    this.initialReconnectDelay = 2000;
    this.currentReconnectDelay = 2000;
    this.maxReconnectDelay = 10000;
    this.hasConnectedBefore = false;
    this.onReconnectCallback = null;
  }

  connect(onMessage, onError, onReconnect = null) {
    console.log('[ws][startup-debug] connect() called');
    // Store the onReconnect callback for use during reconnections
    if (onReconnect) {
      this.onReconnectCallback = onReconnect;
    }
    
    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
    const host = window.location.host;
    const wsUrl = `${protocol}//${host}`;

    console.log('[ws][startup-debug] Connecting to:', wsUrl);

    this.ws = new WebSocket(wsUrl);

    this.ws.onopen = () => {
      console.log('[ws][startup-debug] WebSocket OPENED - connected to dashboard backend');
      
      // Check if this is a reconnection (not the first connection)
      if (this.hasConnectedBefore) {
        console.log('[ws] Reconnection detected, firing onReconnect callback');
        if (this.onReconnectCallback) {
          this.onReconnectCallback();
        }
      } else {
        console.log('[ws][startup-debug] Initial connection established - waiting for initial state from backend');
        this.hasConnectedBefore = true;
      }
      
      this.reconnectAttempts = 0;
      this.currentReconnectDelay = this.initialReconnectDelay;
    };

    this.ws.onmessage = (event) => {
      try {
        const data = JSON.parse(event.data);
        console.log('[ws][startup-debug] Received message type:', data.type, '- currentScene:', data.currentScene);
        onMessage(data);
      } catch (error) {
        console.error('[ws] Error parsing message:', error);
      }
    };

    this.ws.onerror = (error) => {
      console.error('[ws] WebSocket error:', error);
      if (onError) onError(error);
    };

    this.ws.onclose = () => {
      console.log('[ws] Connection closed');
      this.attemptReconnect(onMessage, onError);
    };
  }

  attemptReconnect(onMessage, onError) {
    this.reconnectAttempts++;
    console.log(`[ws] Attempting to reconnect in ${this.currentReconnectDelay / 1000}s (attempt ${this.reconnectAttempts})...`);
    
    setTimeout(() => {
      this.connect(onMessage, onError);
      // Implement exponential backoff with max cap
      this.currentReconnectDelay = Math.min(
        this.currentReconnectDelay * 2,
        this.maxReconnectDelay
      );
    }, this.currentReconnectDelay);
  }

  disconnect() {
    if (this.ws) {
      this.ws.close();
      this.ws = null;
    }
  }

  isConnected() {
    return this.ws && this.ws.readyState === WebSocket.OPEN;
  }
}