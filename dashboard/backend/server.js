const express = require('express');
const http = require('http');
const WebSocket = require('ws');
const cors = require('cors');
const path = require('path');
const fs = require('fs').promises;
const { exec } = require('child_process');
const { promisify } = require('util');
const multer = require('multer');
const AggregatorService = require('./services/aggregator');
const ControllerService = require('./services/controller');
const ControllerWebSocketClient = require('./services/controllerWebSocket');
const SceneService = require('./services/scene');

const execPromise = promisify(exec);

// Configuration from environment variables
const PORT = process.env.PORT || 3000;
const CONTROLLER_API = process.env.CONTROLLER_API || 'http://controller:8089';
const NGINX_STATS = process.env.NGINX_STATS || 'http://nginx-rtmp:8080/stat';
const POLLING_INTERVAL = parseInt(process.env.POLLING_INTERVAL || '2000');
const SRT_PORT = process.env.SRT_PORT || '1937';
const SRT_DOMAIN = process.env.SRT_DOMAIN || 'localhost';
const KICK_CHANNEL = process.env.KICK_CHANNEL || '';
const SHARED_DIR = '/app/shared';
const FALLBACK_CONFIG_PATH = path.join(SHARED_DIR, 'fallback_config.json');
const KICK_CONFIG_PATH = path.join(SHARED_DIR, 'kick_config.json');

console.log('='.repeat(60));
console.log('Stream Dashboard Backend - Starting Up');
console.log('='.repeat(60));
console.log(`[config] Port: ${PORT}`);
console.log(`[config] Controller API: ${CONTROLLER_API}`);
console.log(`[config] Nginx Stats: ${NGINX_STATS}`);
console.log(`[config] Polling Interval: ${POLLING_INTERVAL}ms`);
console.log(`[config] SRT: srt://${SRT_DOMAIN}:${SRT_PORT}`);
console.log('='.repeat(60));

// Initialize services
const aggregator = new AggregatorService({
  controllerUrl: CONTROLLER_API,
  compositorUrl: NGINX_STATS,
  pollingInterval: POLLING_INTERVAL,
  srtPort: SRT_PORT,
  srtDomain: SRT_DOMAIN
});

const controller = new ControllerService(CONTROLLER_API);
const controllerWs = new ControllerWebSocketClient(CONTROLLER_API);
// Note: SceneService not initialized - no compositor in this project
// const sceneService = new SceneService(COMPOSITOR_API);

// Multer configuration for file uploads
const storage = multer.diskStorage({
  destination: async (req, file, cb) => {
    try {
      await fs.mkdir(SHARED_DIR, { recursive: true });
      cb(null, SHARED_DIR);
    } catch (error) {
      cb(error);
    }
  },
  filename: (req, file, cb) => {
    // Use fixed filenames based on file type
    const ext = path.extname(file.originalname).toLowerCase();
    if (file.fieldname === 'image') {
      cb(null, 'offline.png');
    } else if (file.fieldname === 'video') {
      cb(null, 'offline.mp4');
    } else {
      cb(new Error('Invalid field name'));
    }
  }
});

const fileFilter = (req, file, cb) => {
  const allowedImageTypes = ['.png', '.jpg', '.jpeg', '.gif'];
  const allowedVideoTypes = ['.mp4', '.mov', '.mpeg'];
  const ext = path.extname(file.originalname).toLowerCase();
  
  if (file.fieldname === 'image' && allowedImageTypes.includes(ext)) {
    cb(null, true);
  } else if (file.fieldname === 'video' && allowedVideoTypes.includes(ext)) {
    cb(null, true);
  } else {
    cb(new Error(`Invalid file type: ${ext}`));
  }
};

const upload = multer({
  storage: storage,
  fileFilter: fileFilter,
  limits: {
    fileSize: 100 * 1024 * 1024 // 100MB for images
  }
});

const uploadVideo = multer({
  storage: storage,
  fileFilter: fileFilter,
  limits: {
    fileSize: 500 * 1024 * 1024 // 500MB for videos
  }
});

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
  console.log('[ws] Frontend client connected');
  
  // Send latest container state to newly connected client
  if (latestContainerState.containers.length > 0) {
    ws.send(JSON.stringify({
      type: 'container_update',
      containers: latestContainerState.containers,
      timestamp: latestContainerState.timestamp
    }));
  }
  
  // Handle messages from frontend clients
  ws.on('message', (data) => {
    try {
      const message = JSON.parse(data.toString());
      
      if (message.type === 'subscribe_logs') {
        console.log(`[ws] Frontend client subscribing to logs for ${message.container}`);
        controllerWs.subscribeToLogs(message.container, message.lines || 100);
      } else if (message.type === 'unsubscribe_logs') {
        console.log(`[ws] Frontend client unsubscribing from logs for ${message.container}`);
        controllerWs.unsubscribeFromLogs(message.container);
      }
    } catch (error) {
      console.error('[ws] Error handling frontend message:', error.message);
    }
  });
  
  ws.on('close', () => {
    console.log('[ws] Frontend client disconnected');
  });

  ws.on('error', (error) => {
    console.error('[ws] Frontend WebSocket error:', error.message);
  });
});

// Store the latest container state for new connections
let latestContainerState = {
  containers: [],
  timestamp: new Date().toISOString()
};

// Broadcast function to send data to all connected frontend clients
function broadcast(data) {
  const message = JSON.stringify(data);
  let sent = 0;
  
  wss.clients.forEach((client) => {
    if (client.readyState === WebSocket.OPEN) {
      client.send(message);
      sent++;
    }
  });
  
  if (sent > 0 && data.type !== 'new_logs') {
    console.log(`[ws] Broadcasted ${data.type} to ${sent} client(s)`);
  }
}

// Setup controller WebSocket event handlers
controllerWs.on('connected', () => {
  console.log('[main] Controller WebSocket connected');
});

controllerWs.on('disconnected', () => {
  console.error('[main] Controller WebSocket disconnected');
});

controllerWs.on('initial_state', (message) => {
  console.log(`[main] Received initial state from controller`);
  latestContainerState = {
    containers: message.containers,
    timestamp: message.timestamp
  };
  
  // Broadcast to all connected frontend clients
  broadcast({
    type: 'container_update',
    containers: message.containers,
    timestamp: message.timestamp
  });
});

controllerWs.on('status_change', (message) => {
  console.log(`[main] Container status changed: ${message.changes.length} change(s)`);
  
  // Update our state with the changes
  message.changes.forEach(change => {
    const idx = latestContainerState.containers.findIndex(c => c.name === change.name);
    if (idx !== -1) {
      latestContainerState.containers[idx] = {
        ...latestContainerState.containers[idx],
        status: change.currentStatus,
        health: change.currentHealth,
        running: change.running,
        statusDetail: change.statusDetail
      };
    }
  });
  
  latestContainerState.timestamp = message.timestamp;
  
  // Broadcast updated state to frontend
  broadcast({
    type: 'container_update',
    containers: latestContainerState.containers,
    timestamp: message.timestamp,
    changes: message.changes
  });
});

controllerWs.on('new_logs', (message) => {
  // Limit log messages to max 50 lines for performance
  const limitedLogs = message.logs && message.logs.length > 50
    ? message.logs.slice(-50)
    : message.logs;
  
  // Forward log messages to frontend clients
  broadcast({
    type: 'new_logs',
    container: message.container,
    logs: limitedLogs
  });
});

controllerWs.on('log_snapshot', (message) => {
  // Limit log snapshot to max 50 lines for performance
  const limitedLogs = message.logs && message.logs.length > 50
    ? message.logs.slice(-50)
    : message.logs;
  
  // Forward log snapshot to frontend clients
  broadcast({
    type: 'log_snapshot',
    container: message.container,
    logs: limitedLogs
  });
});

controllerWs.on('error', (error) => {
  console.error('[main] Controller WebSocket error:', error.message);
});

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

// Container control endpoints (proxy to controller)
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

// Scene switching endpoints - DISABLED (no compositor in this project)
// These endpoints are kept for API compatibility but return not implemented
app.post('/api/scene/switch', async (req, res) => {
  res.status(501).json({
    error: 'Scene switching not available - no compositor service configured',
    message: 'This feature requires a compositor service which is not part of this deployment'
  });
});

app.get('/api/scene/current', async (req, res) => {
  res.status(501).json({
    error: 'Scene query not available - no compositor service configured'
  });
});

app.get('/api/scene/mode', async (req, res) => {
  res.status(501).json({
    error: 'Scene mode not available - no compositor service configured'
  });
});

// Privacy mode endpoints - DISABLED (no compositor in this project)
app.post('/api/privacy/enable', async (req, res) => {
  res.status(501).json({
    error: 'Privacy mode not available - no compositor service configured'
  });
});

app.post('/api/privacy/disable', async (req, res) => {
  res.status(501).json({
    error: 'Privacy mode not available - no compositor service configured'
  });
});

app.get('/api/privacy', async (req, res) => {
  res.status(501).json({
    error: 'Privacy mode not available - no compositor service configured'
  });
});
// Fallback configuration endpoints

// Get fallback configuration
app.get('/api/fallback/config', async (req, res) => {
  try {
    // Ensure shared directory exists
    await fs.mkdir(SHARED_DIR, { recursive: true });
    
    let config;
    try {
      const data = await fs.readFile(FALLBACK_CONFIG_PATH, 'utf8');
      config = JSON.parse(data);
    } catch (error) {
      // Config doesn't exist, return defaults from environment
      config = {
        source: process.env.FALLBACK_SOURCE || 'BLACK',
        imagePath: '/app/shared/offline.png',
        videoPath: '/app/shared/offline.mp4',
        browserUrl: process.env.OFFLINE_SOURCE_URL || 'https://example.com',
        activeContainer: null,
        lastUpdated: new Date().toISOString()
      };
    }
    
    res.json(config);
  } catch (error) {
    console.error('[api] Error getting fallback config:', error.message);
    res.status(500).json({ error: error.message });
  }
});

// Update fallback configuration
app.post('/api/fallback/config', async (req, res) => {
  try {
    const { source, browserUrl } = req.body;
    
    if (!['BLACK', 'IMAGE', 'VIDEO', 'BROWSER'].includes(source)) {
      return res.status(400).json({ error: 'Invalid source type' });
    }
    
    // Ensure shared directory exists
    await fs.mkdir(SHARED_DIR, { recursive: true });
    
    // Read existing config or create new one
    let config;
    try {
      const data = await fs.readFile(FALLBACK_CONFIG_PATH, 'utf8');
      config = JSON.parse(data);
    } catch (error) {
      config = {
        source: 'BLACK',
        imagePath: '/app/shared/offline.png',
        videoPath: '/app/shared/offline.mp4',
        browserUrl: 'https://example.com',
        activeContainer: null
      };
    }
    
    // Update config
    config.source = source;
    if (browserUrl !== undefined) {
      config.browserUrl = browserUrl;
    }
    config.lastUpdated = new Date().toISOString();
    
    // Get the currently active container before making changes
    const previousActiveContainer = config.activeContainer;
    
    // Manage offline containers based on source
    const containerActions = {
      'BLACK': async () => {
        // Stop the currently active container if any
        if (previousActiveContainer) {
          console.log(`[fallback] Stopping active container: ${previousActiveContainer}`);
          await controller.stopContainer(previousActiveContainer);
        }
        config.activeContainer = null;
      },
      'IMAGE': async () => {
        // Stop the currently active container if any
        if (previousActiveContainer) {
          console.log(`[fallback] Stopping active container: ${previousActiveContainer}`);
          await controller.stopContainer(previousActiveContainer);
        }
        console.log(`[fallback] Starting offline-image container`);
        await controller.startContainer('offline-image');
        config.activeContainer = 'offline-image';
      },
      'VIDEO': async () => {
        // Stop the currently active container if any
        if (previousActiveContainer) {
          console.log(`[fallback] Stopping active container: ${previousActiveContainer}`);
          await controller.stopContainer(previousActiveContainer);
        }
        console.log(`[fallback] Starting offline-video container`);
        await controller.startContainer('offline-video');
        config.activeContainer = 'offline-video';
      },
      'BROWSER': async () => {
        // If browser URL changed and browser is already running, restart it
        const needsRestart = browserUrl !== undefined && previousActiveContainer === 'offline-browser';
        
        if (needsRestart) {
          console.log(`[fallback] Restarting offline-browser due to URL change`);
          await controller.stopContainer('offline-browser');
          // Wait a moment for container to stop
          await new Promise(resolve => setTimeout(resolve, 1000));
          await controller.startContainer('offline-browser');
        } else {
          // Stop the currently active container if it's not browser
          if (previousActiveContainer && previousActiveContainer !== 'offline-browser') {
            console.log(`[fallback] Stopping active container: ${previousActiveContainer}`);
            await controller.stopContainer(previousActiveContainer);
          }
          // Start browser if not already running
          if (previousActiveContainer !== 'offline-browser') {
            console.log(`[fallback] Starting offline-browser container`);
            await controller.startContainer('offline-browser');
          }
        }
        config.activeContainer = 'offline-browser';
      }
    };
    
    // Execute container actions and save config afterwards
    if (containerActions[source]) {
      await containerActions[source]();
    }
    
    // Save config with updated activeContainer after container operations
    await fs.writeFile(FALLBACK_CONFIG_PATH, JSON.stringify(config, null, 2));
    console.log(`[fallback] Configuration saved: source=${source}, activeContainer=${config.activeContainer}`);
    
    res.json({
      success: true,
      config: config,
      message: `Fallback source set to ${source}`
    });
  } catch (error) {
    console.error('[api] Error updating fallback config:', error.message);
    res.status(500).json({ error: error.message });
  }
});

// Upload image file
app.post('/api/fallback/upload-image', upload.single('image'), async (req, res) => {
  try {
    if (!req.file) {
      return res.status(400).json({ error: 'No image file uploaded' });
    }
    
    console.log(`[fallback] Image uploaded: ${req.file.filename} (${req.file.size} bytes)`);
    
    res.json({
      success: true,
      filename: req.file.filename,
      size: req.file.size,
      path: req.file.path
    });
  } catch (error) {
    console.error('[api] Error uploading image:', error.message);
    res.status(500).json({ error: error.message });
  }
});

// Upload video file
app.post('/api/fallback/upload-video', uploadVideo.single('video'), async (req, res) => {
  try {
    if (!req.file) {
      return res.status(400).json({ error: 'No video file uploaded' });
    }
    
    console.log(`[fallback] Video uploaded: ${req.file.filename} (${req.file.size} bytes)`);
    
    // Extract thumbnail from the middle of the video
    const videoPath = req.file.path;
    const thumbnailPath = path.join(SHARED_DIR, 'offline-thumbnail.png');
    
    try {
      // First, get video duration
      const probeCommand = `ffprobe -v error -show_entries format=duration -of default=noprint_wrappers=1:nokey=1 "${videoPath}"`;
      const { stdout: durationOutput } = await execPromise(probeCommand);
      const duration = parseFloat(durationOutput.trim());
      
      if (isNaN(duration) || duration <= 0) {
        throw new Error('Could not determine video duration');
      }
      
      // Extract frame at 50% of video duration
      const seekTime = duration / 2;
      const ffmpegCommand = `ffmpeg -ss ${seekTime} -i "${videoPath}" -vframes 1 -y "${thumbnailPath}"`;
      
      console.log(`[fallback] Extracting thumbnail at ${seekTime.toFixed(2)}s (50% of ${duration.toFixed(2)}s)`);
      await execPromise(ffmpegCommand);
      
      console.log(`[fallback] Thumbnail generated successfully: ${thumbnailPath}`);
    } catch (thumbnailError) {
      console.error('[fallback] Error generating thumbnail:', thumbnailError.message);
      // Continue even if thumbnail generation fails - video is still uploaded
    }
    
    res.json({
      success: true,
      filename: req.file.filename,
      size: req.file.size,
      path: req.file.path,
      thumbnailGenerated: true
    });
  } catch (error) {
    console.error('[api] Error uploading video:', error.message);
    res.status(500).json({ error: error.message });
  }
});

// Serve the uploaded image file
app.get('/api/fallback/image', async (req, res) => {
  try {
    const imagePath = path.join(SHARED_DIR, 'offline.png');
    
    // Check if file exists
    try {
      await fs.access(imagePath);
    } catch (error) {
      return res.status(404).json({ error: 'Image not found' });
    }
    
    // Send the file
    res.sendFile(imagePath);
  } catch (error) {
    console.error('[api] Error serving image:', error.message);
    res.status(500).json({ error: error.message });
  }
});

// Serve the video thumbnail
app.get('/api/fallback/video-thumbnail', async (req, res) => {
  try {
    const thumbnailPath = path.join(SHARED_DIR, 'offline-thumbnail.png');
    
    // Check if file exists
    try {
      await fs.access(thumbnailPath);
    } catch (error) {
      return res.status(404).json({ error: 'Video thumbnail not found' });
    }
    
    // Send the file
    res.sendFile(thumbnailPath);
  } catch (error) {
    console.error('[api] Error serving video thumbnail:', error.message);
    res.status(500).json({ error: error.message });
  }
});

// Kick configuration endpoints

// Get Kick configuration
app.get('/api/kick/config', async (req, res) => {
  try {
    // Ensure shared directory exists
    await fs.mkdir(SHARED_DIR, { recursive: true });
    
    let config;
    let source = 'env'; // Default to environment variables
    
    try {
      const data = await fs.readFile(KICK_CONFIG_PATH, 'utf8');
      config = JSON.parse(data);
      
      // Check if config has valid (non-empty) values
      if (config.kickUrl && config.kickUrl.trim() !== '' &&
          config.kickKey && config.kickKey.trim() !== '') {
        source = 'config';
      } else {
        // Config exists but has empty values, fall back to env
        console.log('[kick] Config file has empty values, using environment variables');
        config = {
          kickUrl: process.env.KICK_URL || '',
          kickKey: process.env.KICK_KEY || '',
          lastUpdated: new Date().toISOString()
        };
      }
    } catch (error) {
      // Config doesn't exist, use environment variables
      console.log('[kick] Config file not found, using environment variables');
      config = {
        kickUrl: process.env.KICK_URL || '',
        kickKey: process.env.KICK_KEY || '',
        lastUpdated: new Date().toISOString()
      };
    }
    
    // Add source metadata to response
    res.json({
      ...config,
      source: source
    });
  } catch (error) {
    console.error('[api] Error getting Kick config:', error.message);
    res.status(500).json({ error: error.message });
  }
});

// Update Kick configuration
app.post('/api/kick/config', async (req, res) => {
  try {
    const { kickUrl, kickKey } = req.body;
    
    if (!kickUrl || !kickKey) {
      return res.status(400).json({ error: 'Both kickUrl and kickKey are required' });
    }
    
    // Ensure shared directory exists
    await fs.mkdir(SHARED_DIR, { recursive: true });
    
    // Create or update config
    const config = {
      kickUrl: kickUrl.trim(),
      kickKey: kickKey.trim(),
      lastUpdated: new Date().toISOString()
    };
    
    // Save config
    await fs.writeFile(KICK_CONFIG_PATH, JSON.stringify(config, null, 2));
    console.log('[kick] Configuration saved successfully');
    
    res.json({
      success: true,
      message: 'Kick configuration saved successfully',
      config: {
        kickUrl: config.kickUrl,
        kickKey: '[REDACTED]',
        lastUpdated: config.lastUpdated
      }
    });
  } catch (error) {
    console.error('[api] Error updating Kick config:', error.message);
    res.status(500).json({ error: error.message });
  }
});

// Delete Kick configuration (reset to defaults)
app.delete('/api/kick/config', async (req, res) => {
  try {
    let wasRunning = false;
    
    // Check if ffmpeg-kick container is running
    try {
      const statusResult = await controller.getContainerStatus('ffmpeg-kick');
      wasRunning = statusResult.status === 'running';
      console.log(`[kick] Container status before reset: ${statusResult.status}`);
    } catch (error) {
      console.log('[kick] Could not check container status:', error.message);
    }
    
    // Delete the config file
    try {
      await fs.unlink(KICK_CONFIG_PATH);
      console.log('[kick] Configuration file deleted');
    } catch (error) {
      if (error.code !== 'ENOENT') {
        throw error;
      }
      console.log('[kick] Config file did not exist');
    }
    
    // Restart container if it was running
    if (wasRunning) {
      console.log('[kick] Restarting container to pick up environment variables...');
      try {
        await controller.restartContainer('ffmpeg-kick');
        console.log('[kick] Container restarted successfully');
      } catch (error) {
        console.error('[kick] Error restarting container:', error.message);
        // Continue anyway - config was deleted
      }
    }
    
    // Return default values from environment
    const defaultConfig = {
      kickUrl: process.env.KICK_URL || '',
      kickKey: process.env.KICK_KEY || '',
      source: 'env',
      lastUpdated: new Date().toISOString()
    };
    
    res.json({
      success: true,
      message: 'Configuration reset to defaults from environment variables',
      config: defaultConfig,
      containerRestarted: wasRunning
    });
  } catch (error) {
    console.error('[api] Error resetting Kick config:', error.message);
    res.status(500).json({ error: error.message });
  }
});

// Start Kick stream
app.post('/api/kick/start', async (req, res) => {
  try {
    // Check if config file exists to log which configuration source will be used
    let configSource = 'environment variables';
    try {
      await fs.access(KICK_CONFIG_PATH);
      configSource = 'config file';
    } catch (error) {
      // Config file not found, container will use environment variables
      console.log('[kick] Config file not found, container will use environment variables (KICK_URL and KICK_KEY)');
    }
    
    // Start the ffmpeg-kick container (it will use config file OR environment variables)
    const result = await controller.startContainer('ffmpeg-kick');
    console.log(`[kick] Starting Kick stream using ${configSource}`);
    res.json(result);
  } catch (error) {
    console.error('[api] Error starting Kick stream:', error.message);
    res.status(500).json({ error: error.message });
  }
});

// Stop Kick stream
app.post('/api/kick/stop', async (req, res) => {
  try {
    const result = await controller.stopContainer('ffmpeg-kick');
    console.log('[kick] Stopping Kick stream');
    res.json(result);
  } catch (error) {
    console.error('[api] Error stopping Kick stream:', error.message);
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
  console.log('[http]   GET  /api/scene/current (uses compositor /health endpoint)');
  console.log('[http]   GET  /api/scene/mode');
  console.log('[http]   POST /api/scene/camera');
  console.log('[http]   POST /api/scene/privacy');
  console.log('[ws] WebSocket server ready for frontend clients');
  
  // Connect to controller WebSocket
  controllerWs.connect();
  console.log('[main] Connecting to controller WebSocket...');
  console.log('[main] Dashboard backend is now running');
});

// Graceful shutdown
process.on('SIGTERM', () => {
  console.log('[main] SIGTERM received, shutting down gracefully');
  
  // Disconnect from controller WebSocket
  controllerWs.disconnect();
  
  // Close all frontend WebSocket connections immediately
  console.log(`[main] Closing ${wss.clients.size} frontend WebSocket connection(s)`);
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
  
  // Disconnect from controller WebSocket
  controllerWs.disconnect();
  
  // Close all frontend WebSocket connections immediately
  console.log(`[main] Closing ${wss.clients.size} frontend WebSocket connection(s)`);
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