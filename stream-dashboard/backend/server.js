const express = require('express');
const http = require('http');
const WebSocket = require('ws');
const cors = require('cors');
const path = require('path');
const AggregatorService = require('./services/aggregator');
const ControllerService = require('./services/controller');
const SceneService = require('./services/scene');

// Configuration from environment variables
const PORT = process.env.PORT || 3000;
const CONTROLLER_API = process.env.CONTROLLER_API || 'http://stream-controller:8089';
const COMPOSITOR_API = process.env.COMPOSITOR_API || 'http://compositor:8088';
const POLLING_INTERVAL = parseInt(process.env.POLLING_INTERVAL || '2000');
const SRT_PORT = process.env.SRT_PORT || '1937';
const SRT_DOMAIN = process.env.SRT_DOMAIN || 'localhost';
const KICK_CHANNEL = process.env.KICK_CHANNEL || '';

console.log('='.repeat(60));
console.log('Stream Dashboard Backend - Starting Up');
console.log('='.repeat(60));
console.log(`[config] Port: ${PORT}`);
console.log(`[config] Controller API: ${CONTROLLER_API}`);
console.log(`[config] Compositor API: ${COMPOSITOR_API}`);
console.log(`[config] Polling Interval: ${POLLING_INTERVAL}ms`);
console.log(`[config] SRT: srt://${SRT_DOMAIN}:${SRT_PORT}`);
console.log('='.repeat(60));

// Initialize services
const aggregator = new AggregatorService({
  controllerUrl: CONTROLLER_API,
  compositorUrl: COMPOSITOR_API,
  pollingInterval: POLLING_INTERVAL,
  srtPort: SRT_PORT,
  srtDomain: SRT_DOMAIN
});

const controller = new ControllerService(CONTROLLER_API);
const sceneService = new SceneService(COMPOSITOR_API);

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

// Get SRT configuration
app.get('/api/config', async (req, res) => {
  try {
    res.json({
      srtUrl: `srt://${SRT_DOMAIN}:${SRT_PORT}`,
      srtPort: SRT_PORT,
      srtDomain: SRT_DOMAIN,
      protocol: 'srt',
      kickChannel: KICK_CHANNEL
    });
  } catch (error) {
    console.error('[api] Error getting config:', error.message);
    res.status(500).json({ error: error.message });
  }
});

// Container control endpoints (proxy to stream-controller)
app.get('/api/container/:name/logs', async (req, res) => {
  try {
    const tail = req.query.tail || 500;
    const result = await controller.getContainerLogs(req.params.name, tail);
    res.json(result);
  } catch (error) {
    res.status(500).json({ error: error.message });
  }
});

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

app.post('/api/container/:name/create-and-start', async (req, res) => {
  try {
    const result = await controller.createAndStartContainer(req.params.name);
    res.json(result);
  } catch (error) {
    res.status(500).json({ error: error.message });
  }
});

// Scene switching endpoints (proxy to srt-switcher)
app.post('/api/scene/switch', async (req, res) => {
  try {
    const { scene } = req.body;
    if (!scene) {
      return res.status(400).json({ error: 'Scene name is required' });
    }
    const result = await sceneService.switchScene(scene);
    res.json(result);
  } catch (error) {
    res.status(500).json({ error: error.message });
  }
});

app.get('/api/scene/current', async (req, res) => {
  try {
    const result = await sceneService.getCurrentScene();
    res.json(result);
  } catch (error) {
    res.status(500).json({ error: error.message });
  }
});

app.get('/api/scene/mode', async (req, res) => {
  try {
    const result = await sceneService.getSceneMode();
    res.json(result);
  } catch (error) {
    res.status(500).json({ error: error.message });
  }
});

// Privacy mode endpoints (proxy to compositor)
app.post('/api/privacy/enable', async (req, res) => {
  try {
    const result = await sceneService.setPrivacyMode(true);
    res.json(result);
  } catch (error) {
    res.status(500).json({ error: error.message });
  }
});

app.post('/api/privacy/disable', async (req, res) => {
  try {
    const result = await sceneService.setPrivacyMode(false);
    res.json(result);
  } catch (error) {
    res.status(500).json({ error: error.message });
  }
});

app.get('/api/privacy', async (req, res) => {
  try {
    const result = await sceneService.getPrivacyMode();
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
  console.log('[http]   GET  /api/config');
  console.log('[http]   POST /api/container/:name/start');
  console.log('[http]   POST /api/container/:name/stop');
  console.log('[http]   POST /api/container/:name/restart');
  console.log('[http]   POST /api/scene/switch');
  console.log('[http]   GET  /api/scene/current');
  console.log('[http]   GET  /api/scene/mode');
  console.log('[http]   POST /api/scene/camera');
  console.log('[http]   POST /api/scene/privacy');
  console.log('[ws] WebSocket server ready');
  
  // Start polling and broadcasting
  aggregator.startPolling(broadcast);
  console.log('[main] Dashboard backend is now running');
});

// Graceful shutdown
process.on('SIGTERM', () => {
  console.log('[main] SIGTERM received, shutting down gracefully');
  aggregator.stopPolling();
  
  // Close all WebSocket connections immediately
  console.log(`[main] Closing ${wss.clients.size} WebSocket connection(s)`);
  wss.clients.forEach(client => client.terminate());
  
  // Try to close server gracefully, but force exit after 1 second
  const forceExitTimer = setTimeout(() => {
    console.log('[main] Force exiting after timeout');
    process.exit(0);
  }, 1000);
  
  server.close(() => {
    clearTimeout(forceExitTimer);
    console.log('[main] Server closed');
    process.exit(0);
  });
});

process.on('SIGINT', () => {
  console.log('[main] SIGINT received, shutting down gracefully');
  aggregator.stopPolling();
  
  // Close all WebSocket connections immediately
  console.log(`[main] Closing ${wss.clients.size} WebSocket connection(s)`);
  wss.clients.forEach(client => client.terminate());
  
  // Try to close server gracefully, but force exit after 1 second
  const forceExitTimer = setTimeout(() => {
    console.log('[main] Force exiting after timeout');
    process.exit(0);
  }, 1000);
  
  server.close(() => {
    clearTimeout(forceExitTimer);
    console.log('[main] Server closed');
    process.exit(0);
  });
});