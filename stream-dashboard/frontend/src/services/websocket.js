export class WebSocketService {
  constructor() {
    this.ws = null;
    this.listeners = [];
    this.reconnectAttempts = 0;
    this.initialReconnectDelay = 2000;
    this.currentReconnectDelay = 2000;
    this.maxReconnectDelay = 10000;
  }

  connect(onMessage, onError) {
    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
    const host = window.location.host;
    const wsUrl = `${protocol}//${host}`;

    console.log('[ws] Connecting to:', wsUrl);

    this.ws = new WebSocket(wsUrl);

    this.ws.onopen = () => {
      console.log('[ws] Connected');
      this.reconnectAttempts = 0;
      this.currentReconnectDelay = this.initialReconnectDelay;
    };

    this.ws.onmessage = (event) => {
      try {
        const data = JSON.parse(event.data);
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