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
const metricsService = require('./services/metrics');

const execPromise = promisify(exec);

// Configuration from environment variables
const PORT = process.env.PORT || 3000;
const CONTROLLER_API = process.env.CONTROLLER_API || 'http://controller:8089';
const NGINX_STATS = process.env.NGINX_STATS || 'http://nginx-rtmp:8080/stat';
const POLLING_INTERVAL = parseInt(process.env.POLLING_INTERVAL || '2000');
const SRT_PORT = process.env.SRT_PORT || '1937';
const SRT_DOMAIN = process.env.SRT_DOMAIN || 'localhost';
const RTMP_PORT = process.env.RTMP_PORT || '1935';
const KICK_CHANNEL = process.env.KICK_CHANNEL || '';
const NGINX_RTMP_HLS = process.env.NGINX_RTMP_HLS || 'http://nginx-rtmp:8080';
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
console.log(`[config] RTMP Preview: rtmp://${SRT_DOMAIN}:${RTMP_PORT}/live/stream`);
console.log('='.repeat(60));

const controller = new ControllerService(CONTROLLER_API);
const controllerWs = new ControllerWebSocketClient(CONTROLLER_API);

// Initialize aggregator with reference to controller WebSocket for scene state
const aggregator = new AggregatorService({
  controllerUrl: CONTROLLER_API,
  compositorUrl: NGINX_STATS,
  pollingInterval: POLLING_INTERVAL,
  srtPort: SRT_PORT,
  srtDomain: SRT_DOMAIN,
  controllerWs: controllerWs  // Pass reference to get scene state
});
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
  
  // Send latest container state to newly connected client with current scene/privacy
  if (latestContainerState.containers.length > 0) {
    const sceneState = controllerWs.getSceneState();
    console.log(`[ws] Sending initial state to new client: scene=${sceneState.currentScene}, privacy=${sceneState.privacyEnabled}`);
    ws.send(JSON.stringify({
      type: 'container_update',
      containers: latestContainerState.containers,
      timestamp: latestContainerState.timestamp,
      currentScene: sceneState.currentScene,
      privacyEnabled: sceneState.privacyEnabled
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
  console.log(`[main] Initial scene: ${message.current_scene}, privacy: ${message.privacy_enabled}`);
  latestContainerState = {
    containers: message.containers,
    timestamp: message.timestamp
  };
  
  // Broadcast to all connected frontend clients with scene info
  broadcast({
    type: 'container_update',
    containers: message.containers,
    timestamp: message.timestamp,
    currentScene: message.current_scene,
    privacyEnabled: message.privacy_enabled
  });
});

controllerWs.on('status_change', (message) => {
  console.log(`[main] Container status changed: ${message.changes.length} change(s)`);
  if (message.current_scene !== undefined) {
    console.log(`[main] Status includes scene: ${message.current_scene}, privacy: ${message.privacy_enabled}`);
  }
  
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
  
  // Broadcast updated state to frontend with scene info
  broadcast({
    type: 'container_update',
    containers: latestContainerState.containers,
    timestamp: message.timestamp,
    changes: message.changes,
    currentScene: message.current_scene,
    privacyEnabled: message.privacy_enabled
  });
});

controllerWs.on('new_logs', (message) => {
  // Skip if no logs to send
  if (!message.logs || message.logs.length === 0) {
    return;
  }
  
  // Limit log messages to max 50 lines for performance
  const limitedLogs = message.logs.length > 50
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

controllerWs.on('scene_change', (data) => {
  console.log(`[main] Scene changed to: ${data.currentScene}`);
  console.log(`[scene_change_debug] server.js received scene_change event: ${JSON.stringify(data)}`);
  console.log(`[scene_change_debug] Broadcasting to ${wss.clients.size} frontend client(s)`);
  broadcast({
    type: 'scene_change',
    currentScene: data.currentScene,
    privacyEnabled: data.privacyEnabled,
    changeData: data.changeData,
    timestamp: data.timestamp
  });
  console.log(`[scene_change_debug] Broadcast complete for scene_change`);
});

controllerWs.on('privacy_change', (data) => {
  console.log(`[main] Privacy mode changed to: ${data.privacyEnabled}`);
  console.log(`[scene_change_debug] server.js received privacy_change event: ${JSON.stringify(data)}`);
  console.log(`[scene_change_debug] Broadcasting to ${wss.clients.size} frontend client(s)`);
  broadcast({
    type: 'privacy_change',
    privacyEnabled: data.privacyEnabled,
    currentScene: data.currentScene,
    changeData: data.changeData,
    timestamp: data.timestamp
  });
  console.log(`[scene_change_debug] Broadcast complete for privacy_change`);
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
      previewUrl: `rtmp://${SRT_DOMAIN}:${RTMP_PORT}/live/stream`,
      hlsUrl: '/api/hls/stream.m3u8',
      rtmpPort: RTMP_PORT,
      kickChannel: KICK_CHANNEL
    });
  } catch (error) {
    console.error('[api] Error getting config:', error.message);
    res.status(500).json({ error: error.message });
  }
});

// HLS Proxy - forwards requests to nginx-rtmp server
app.get('/api/hls/*', async (req, res) => {
  const hlsPath = req.params[0];
  const targetUrl = `${NGINX_RTMP_HLS}/hls/${hlsPath}`;
  
  try {
    const controller = new AbortController();
    const timeoutId = setTimeout(() => controller.abort(), 10000);
    
    const response = await fetch(targetUrl, {
      method: 'GET',
      signal: controller.signal
    });
    clearTimeout(timeoutId);
    
    if (!response.ok) {
      return res.status(response.status).send('HLS stream not available');
    }
    
    // Set appropriate content-type headers
    const contentType = response.headers.get('content-type');
    if (contentType) {
      res.setHeader('Content-Type', contentType);
    } else if (hlsPath.endsWith('.m3u8')) {
      res.setHeader('Content-Type', 'application/vnd.apple.mpegurl');
    } else if (hlsPath.endsWith('.ts')) {
      res.setHeader('Content-Type', 'video/mp2t');
    }
    
    // Set CORS and caching headers
    res.setHeader('Access-Control-Allow-Origin', '*');
    res.setHeader('Cache-Control', 'no-cache');
    
    // Stream the response body
    const arrayBuffer = await response.arrayBuffer();
    res.send(Buffer.from(arrayBuffer));
  } catch (error) {
    if (error.name === 'AbortError') {
      console.error('[hls-proxy] Request timeout for:', hlsPath);
      return res.status(504).send('HLS proxy timeout');
    }
    console.error('[hls-proxy] Error proxying HLS:', error.message);
    res.status(502).send('HLS proxy error');
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

// Scene endpoints - proxy to controller
app.get('/api/scene/current', async (req, res) => {
  try {
    // Get scene from controller WebSocket client state
    const sceneState = controllerWs.getSceneState();
    res.json({
      scene: sceneState.currentScene,
      privacy_enabled: sceneState.privacyEnabled,
      source: 'controller_websocket'
    });
  } catch (error) {
    console.error('[api] Error getting current scene:', error.message);
    res.status(500).json({ error: error.message });
  }
});

// Scene switching not supported - scenes are controlled by multiplexer
app.post('/api/scene/switch', async (req, res) => {
  res.status(501).json({
    error: 'Scene switching not available',
    message: 'Scenes are automatically controlled by the multiplexer based on live stream availability'
  });
});

app.get('/api/scene/mode', async (req, res) => {
  res.status(501).json({
    error: 'Scene mode not available - use /api/scene/current instead'
  });
});

// Privacy mode endpoints - proxy to controller REST API
app.post('/api/privacy/enable', async (req, res) => {
  try {
    const result = await controller.enablePrivacyMode();
    res.json(result);
  } catch (error) {
    console.error('[api] Error enabling privacy mode:', error.message);
    res.status(500).json({ error: error.message });
  }
});

app.post('/api/privacy/disable', async (req, res) => {
  try {
    const result = await controller.disablePrivacyMode();
    res.json(result);
  } catch (error) {
    console.error('[api] Error disabling privacy mode:', error.message);
    res.status(500).json({ error: error.message });
  }
});

app.get('/api/privacy', async (req, res) => {
  try {
    const result = await controller.getPrivacyMode();
    res.json(result);
  } catch (error) {
    console.error('[api] Error getting privacy mode:', error.message);
    res.status(500).json({ error: error.message });
  }
});
// GitHub last commit endpoint with caching
let cachedCommitData = null;
let cacheTimestamp = null;
const CACHE_DURATION_MS = 5 * 60 * 1000; // 5 minutes

app.get('/api/github/last-commit', async (req, res) => {
  try {
    const now = Date.now();
    
    // Check if cache is valid
    if (cachedCommitData && cacheTimestamp && (now - cacheTimestamp) < CACHE_DURATION_MS) {
      console.log('[github] Returning cached commit data');
      return res.json(cachedCommitData);
    }
    
    // Fetch from GitHub API
    console.log('[github] Fetching latest commit from GitHub API...');
    const controller = new AbortController();
    const timeoutId = setTimeout(() => controller.abort(), 10000);
    
    const response = await fetch('https://api.github.com/repos/nulligun/latestrelayer/commits?per_page=1', {
      headers: {
        'Accept': 'application/vnd.github.v3+json',
        'User-Agent': 'LatestRelayer-Dashboard'
      },
      signal: controller.signal
    });
    clearTimeout(timeoutId);
    
    if (!response.ok) {
      throw new Error(`GitHub API returned ${response.status}`);
    }
    
    const commits = await response.json();
    
    if (!commits || commits.length === 0) {
      throw new Error('No commits found');
    }
    
    const latestCommit = commits[0];
    const commitData = {
      sha: latestCommit.sha,
      message: latestCommit.commit.message.split('\n')[0], // First line only
      date: latestCommit.commit.committer.date,
      author: latestCommit.commit.author.name,
      url: latestCommit.html_url
    };
    
    // Update cache
    cachedCommitData = commitData;
    cacheTimestamp = now;
    
    console.log(`[github] Latest commit: ${commitData.sha.substring(0, 7)} - ${commitData.message}`);
    res.json(commitData);
  } catch (error) {
    if (error.name === 'AbortError') {
      console.error('[github] Request timeout');
      return res.status(504).json({ error: 'GitHub API timeout' });
    }
    console.error('[github] Error fetching commit:', error.message);
    
    // Return cached data if available, even if expired
    if (cachedCommitData) {
      console.log('[github] Returning stale cached data due to error');
      return res.json({ ...cachedCommitData, stale: true });
    }
    
    res.status(500).json({ error: error.message });
  }
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
    
    // Restart ffmpeg-fallback container to pick up the new configuration
    try {
      console.log(`[fallback] Restarting ffmpeg-fallback container to apply new fallback source...`);
      await controller.restartContainer('ffmpeg-fallback');
      console.log(`[fallback] ffmpeg-fallback container restart initiated`);
    } catch (restartError) {
      console.error('[fallback] Error restarting ffmpeg-fallback container:', restartError.message);
      // Continue anyway - config was saved, container can be manually restarted
    }
    
    res.json({
      success: true,
      config: config,
      message: `Fallback source set to ${source}`,
      fallbackContainerRestarted: true
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
    
    // Convert image to MPEG-TS format for streaming
    const imagePath = req.file.path;
    const tsOutputPath = path.join(SHARED_DIR, 'static-image.ts');
    const thumbnailPath = path.join(SHARED_DIR, 'offline-thumbnail.png');
    
    try {
      // Copy the image as the thumbnail (for static images, the image IS the thumbnail)
      await fs.copyFile(imagePath, thumbnailPath);
      console.log(`[fallback] Created thumbnail from static image: ${thumbnailPath}`);
      
      // Convert image to a 30-second MPEG-TS video with audio
      // Uses same format as convert-fallback.sh for compatibility
      // Scale to 1280x720 with letterboxing to maintain aspect ratio
      const ffmpegCommand = `ffmpeg -y -loop 1 -i "${imagePath}" \
        -f lavfi -i "anullsrc=channel_layout=stereo:sample_rate=48000" \
        -vf "scale=1280:720:force_original_aspect_ratio=decrease,pad=1280:720:(ow-iw)/2:(oh-ih)/2:black" \
        -c:v libx264 -preset fast -crf 23 \
        -g 30 -keyint_min 30 -sc_threshold 0 \
        -r 30 -pix_fmt yuv420p \
        -c:a aac -b:a 128k \
        -bsf:v h264_mp4toannexb \
        -f mpegts \
        -mpegts_flags +resend_headers \
        -mpegts_service_id 1 \
        -mpegts_pmt_start_pid 256 \
        -mpegts_start_pid 257 \
        -muxrate 10000000 \
        -t 30 \
        "${tsOutputPath}"`;
      
      console.log(`[fallback] Converting image to MPEG-TS format...`);
      await execPromise(ffmpegCommand);
      console.log(`[fallback] Successfully converted image to: ${tsOutputPath}`);
      
    } catch (conversionError) {
      console.error('[fallback] Error converting image to MPEG-TS:', conversionError.message);
      // Continue anyway - the image is uploaded, conversion can be retried
    }
    
    res.json({
      success: true,
      filename: req.file.filename,
      size: req.file.size,
      path: req.file.path,
      tsConverted: true
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
    const tsOutputPath = path.join(SHARED_DIR, 'video.ts');
    
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
      const thumbnailCommand = `ffmpeg -ss ${seekTime} -i "${videoPath}" -vframes 1 -y "${thumbnailPath}"`;
      
      console.log(`[fallback] Extracting thumbnail at ${seekTime.toFixed(2)}s (50% of ${duration.toFixed(2)}s)`);
      await execPromise(thumbnailCommand);
      
      console.log(`[fallback] Thumbnail generated successfully: ${thumbnailPath}`);
    } catch (thumbnailError) {
      console.error('[fallback] Error generating thumbnail:', thumbnailError.message);
      // Continue even if thumbnail generation fails - video is still uploaded
    }
    
    // Convert video to MPEG-TS format for streaming
    try {
      // Uses same format as convert-fallback.sh for compatibility
      // Scale to 1280x720 with letterboxing to maintain aspect ratio
      const tsCommand = `ffmpeg -y -i "${videoPath}" \
        -vf "scale=1280:720:force_original_aspect_ratio=decrease,pad=1280:720:(ow-iw)/2:(oh-ih)/2:black" \
        -c:v libx264 -preset fast -crf 23 \
        -g 30 -keyint_min 30 -sc_threshold 0 \
        -c:a aac -b:a 128k \
        -bsf:v h264_mp4toannexb \
        -f mpegts \
        -mpegts_flags +resend_headers \
        -mpegts_service_id 1 \
        -mpegts_pmt_start_pid 256 \
        -mpegts_start_pid 257 \
        -muxrate 10000000 \
        "${tsOutputPath}"`;
      
      console.log(`[fallback] Converting video to MPEG-TS format...`);
      await execPromise(tsCommand);
      console.log(`[fallback] Successfully converted video to: ${tsOutputPath}`);
      
    } catch (conversionError) {
      console.error('[fallback] Error converting video to MPEG-TS:', conversionError.message);
      // Continue anyway - the video is uploaded, conversion can be retried
    }
    
    res.json({
      success: true,
      filename: req.file.filename,
      size: req.file.size,
      path: req.file.path,
      thumbnailGenerated: true,
      tsConverted: true
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

// Start system metrics polling interval
let metricsIntervalId = null;

function startMetricsPolling() {
  console.log(`[metrics] Starting system metrics polling every ${POLLING_INTERVAL}ms`);
  
  const pollMetrics = async () => {
    try {
      const systemMetrics = await metricsService.getSystemMetrics();
      broadcast({
        type: 'metrics_update',
        systemMetrics,
        timestamp: new Date().toISOString()
      });
    } catch (error) {
      console.error('[metrics] Error collecting system metrics:', error.message);
    }
  };
  
  // Initial poll
  pollMetrics();
  
  // Set up interval
  metricsIntervalId = setInterval(pollMetrics, POLLING_INTERVAL);
}

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
  
  // Start system metrics polling
  startMetricsPolling();
  
  console.log('[main] Dashboard backend is now running');
});

// Graceful shutdown
process.on('SIGTERM', () => {
  console.log('[main] SIGTERM received, shutting down gracefully');
  
  // Stop metrics polling
  if (metricsIntervalId) {
    clearInterval(metricsIntervalId);
    console.log('[main] Metrics polling stopped');
  }
  
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
  
  // Stop metrics polling
  if (metricsIntervalId) {
    clearInterval(metricsIntervalId);
    console.log('[main] Metrics polling stopped');
  }
  
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