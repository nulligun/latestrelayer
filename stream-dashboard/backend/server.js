const express = require('express');
const http = require('http');
const WebSocket = require('ws');
const cors = require('cors');
const path = require('path');
const AggregatorService = require('./services/aggregator');
const ControllerService = require('./services/controller');

// Configuration from environment variables
const PORT = process.env.PORT || 3000;
const CONTROLLER_API = process.env.CONTROLLER_API || 'http://stream-controller:8089';
const SWITCHER_API = process.env.SWITCHER_API || 'http://stream-switcher:8088';
const NGINX_STATS = process.env.NGINX_STATS || 'http://nginx-rtmp:8080/stat';
const POLLING_INTERVAL = parseInt(process.env.POLLING_INTERVAL || '2000');

console.log('='.repeat(60));
console.log('Stream Dashboard Backend - Starting Up');
console.log('='.repeat(60));
console.log(`[config] Port: ${PORT}`);
console.log(`[config] Controller API: ${CONTROLLER_API}`);
console.log(`[config] Switcher API: ${SWITCHER_API}`);
console.log(`[config] NGINX Stats: ${NGINX_STATS}`);
console.log(`[config] Polling Interval: ${POLLING_INTERVAL}ms`);
console.log('='.repeat(60));

// Initialize services
const aggregator = new AggregatorService({
  controllerUrl: CONTROLLER_API,
  switcherUrl: SWITCHER_API,
  nginxStatsUrl: NGINX_STATS,
  pollingInterval: POLLING_INTERVAL
});

const controller = new ControllerService(CONTROLLER_API);

// Create Express app
const app = express();
app.use(cors());
app.use(express.json());

// Serve static files from frontend build
app.use(express.static(path.join(__dirname, 'frontend/dist')));

// Create HTTP server
const server = http.createServer(app);

// Create WebSocket server
const wss = new WebSocket.Server({ server });

// WebSocket connection handling
wss.on('connection', (ws) => {
  console.log('[ws] Client connected');
  
  ws.on('close', () => {
    console.log('[ws] Client disconnected');
  });

  ws.on('error', (error) => {
    console.error('[ws] WebSocket error:', error.message);
  });
});

// Broadcast function for aggregator
function broadcast(data) {
  const message = JSON.stringify(data);
  let sent = 0;
  
  wss.clients.forEach((client) => {
    if (client.readyState === WebSocket.OPEN) {
      client.send(message);
      sent++;
    }
  });
  
  if (sent > 0) {
    console.log(`[ws] Broadcasted update to ${sent} client(s)`);
  }
}

// REST API endpoints

// Health check
app.get('/api/health', (req, res) => {
  res.json({ status: 'ok', timestamp: new Date().toISOString() });
});

// Get current aggregated data (one-time fetch)
app.get('/api/data', async (req, res) => {
  try {
    const data = await aggregator.aggregateData();
    res.json(data);
  } catch (error) {
    console.error('[api] Error getting data:', error.message);
    res.status(500).json({ error: error.message });
  }
});

// Container control endpoints (proxy to stream-controller)
app.post('/api/container/:name/start', async (req, res) => {
  try {
    const result = await controller.startContainer(req.params.name);
    res.json(result);
  } catch (error) {
    res.status(500).json({ error: error.message });
  }
});

app.post('/api/container/:name/stop', async (req, res) => {
  try {
    const result = await controller.stopContainer(req.params.name);
    res.json(result);
  } catch (error) {
    res.status(500).json({ error: error.message });
  }
});

app.post('/api/container/:name/restart', async (req, res) => {
  try {
    const result = await controller.restartContainer(req.params.name);
    res.json(result);
  } catch (error) {
    res.status(500).json({ error: error.message });
  }
});

// Serve frontend for all other routes (SPA fallback)
app.get('*', (req, res) => {
  res.sendFile(path.join(__dirname, 'frontend/dist/index.html'));
});

// Start server
server.listen(PORT, '0.0.0.0', () => {
  console.log(`[http] Server listening on port ${PORT}`);
  console.log('[http] REST API endpoints:');
  console.log('[http]   GET  /api/health');
  console.log('[http]   GET  /api/data');
  console.log('[http]   POST /api/container/:name/start');
  console.log('[http]   POST /api/container/:name/stop');
  console.log('[http]   POST /api/container/:name/restart');
  console.log('[ws] WebSocket server ready');
  
  // Start polling and broadcasting
  aggregator.startPolling(broadcast);
  console.log('[main] Dashboard backend is now running');
});

// Graceful shutdown
process.on('SIGTERM', () => {
  console.log('[main] SIGTERM received, shutting down gracefully');
  aggregator.stopPolling();
  server.close(() => {
    console.log('[main] Server closed');
    process.exit(0);
  });
});

process.on('SIGINT', () => {
  console.log('[main] SIGINT received, shutting down gracefully');
  aggregator.stopPolling();
  server.close(() => {
    console.log('[main] Server closed');
    process.exit(0);
  });
});