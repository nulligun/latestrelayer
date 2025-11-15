# FFmpeg-Kick Integration Testing Guide

This guide walks through testing the complete Kick streaming implementation.

## Prerequisites

Before testing, ensure:
- Docker and Docker Compose are installed
- The compositor is running and receiving video
- You have valid Kick.com streaming credentials (RTMPS URL and Stream Key)

## Test Plan

### Phase 1: Container Build and Configuration

**1.1 Build the new container**
```bash
docker compose build ffmpeg-kick
```

**Expected:** Build completes successfully without errors.

**1.2 Verify container configuration**
```bash
docker compose config | grep -A 20 ffmpeg-kick
```

**Expected:** Shows container with:
- `profiles: [manual]`
- `restart: "no"`
- Volume mount to shared folder
- Environment variables set correctly

### Phase 2: Dashboard Configuration

**2.1 Access the dashboard**
```bash
# Start the dashboard if not running
docker compose up -d stream-dashboard

# Open in browser
# http://localhost:3000
```

**2.2 Locate Kick Settings section**

**Expected:** New "Kick Settings" section appears between Stream Controls and Container Grid with:
- Kick Stream URL field (obscured)
- Kick Stream Key field (obscured)
- Eye icons to reveal credentials
- Save Configuration button

**2.3 Test credential visibility toggle**

1. Click on the URL field - it should remain obscured
2. Click the eye icon next to URL field
   - **Expected:** URL becomes visible (type="text")
3. Click eye icon again
   - **Expected:** URL becomes obscured again (type="password")
4. Repeat for Stream Key field

**2.4 Enter credentials**

1. Reveal URL field using eye icon
2. Enter: `rtmps://fa723fc1b171.global-contribute.live-video.net/app`
3. Reveal Key field using eye icon
4. Enter your stream key (starts with `sk_`)
5. Click "Save Configuration"

**Expected:**
- Success message appears: "✓ Configuration saved successfully"
- Fields auto-hide after 3 seconds (for security)
- No errors in browser console

**2.5 Verify configuration saved**
```bash
cat shared/kick_config.json
```

**Expected:** File contains your credentials in JSON format with:
- `kickUrl`: Your RTMPS URL
- `kickKey`: Your stream key
- `lastUpdated`: Current timestamp

### Phase 3: Backend API Testing

**3.1 Test GET /api/kick/config**
```bash
curl http://localhost:3000/api/kick/config | jq
```

**Expected:** Returns your configuration (key is not redacted in API response)

**3.2 Test POST /api/kick/config**
```bash
curl -X POST http://localhost:3000/api/kick/config \
  -H "Content-Type: application/json" \
  -d '{
    "kickUrl": "rtmps://test.example.com/app",
    "kickKey": "sk_test_key"
  }' | jq
```

**Expected:** 
- Returns success response
- Updates `shared/kick_config.json`

**3.3 Test start endpoint (dry run)**
```bash
curl -X POST http://localhost:3000/api/kick/start | jq
```

**Expected:** Returns starting status (202 Accepted)

**3.4 Check container status**
```bash
docker compose ps ffmpeg-kick
```

**Expected:** Container should be starting/running

**3.5 Test stop endpoint**
```bash
curl -X POST http://localhost:3000/api/kick/stop | jq
```

**Expected:** 
- Returns stopping status (202 Accepted)
- Container stops gracefully

### Phase 4: Full Dashboard UI Flow

**4.1 Start from Simplified View**

1. Open dashboard: http://localhost:3000
2. Click **Go Live** button

**Expected:**
- Confirmation modal appears
- Click confirm
- ffmpeg-kick container starts
- "Stream to Kick" toggle shows ON
- Status changes to "Live on Kick"

**4.2 Monitor stream health**

1. Check container logs:
```bash
docker compose logs -f ffmpeg-kick
```

**Expected logs:**
- "Configuration loaded successfully"
- "Kick URL: rtmps://..."
- "Stream Key: [REDACTED]"
- "Starting ffmpeg stream to Kick..."
- No error messages

2. Check health status:
```bash
docker compose ps ffmpeg-kick
```

**Expected:** Status shows "healthy" after ~10 seconds

**4.3 Verify on Kick.com**

1. Open Kick.com dashboard
2. Navigate to your channel
3. Check stream status

**Expected:** Stream shows as live with video/audio

**4.4 Stop streaming**

1. Click **End Stream** button in dashboard
2. Confirm in modal

**Expected:**
- Container stops gracefully
- Logs show clean shutdown
- Kick shows stream ended

### Phase 5: Full Interface Testing

**5.1 Switch to Full View**

1. Click toggle icon (⚡/📊) in header
2. Navigate to "Kick Settings" section

**Expected:** Settings section appears with saved credentials (obscured)

**5.2 Test Stream to Kick toggle**

1. Locate "Stream to Kick" toggle in Stream Controls
2. Click to enable

**Expected:**
- Confirmation modal: "GO LIVE ON KICK?"
- Proper warning message
- After confirm: container starts

**5.3 Verify container grid**

**Expected:** 
- ffmpeg-kick appears in container list
- Shows status: running
- Health indicator: healthy
- Can view logs via "View Logs" button

### Phase 6: Error Handling Tests

**6.1 Test without configuration**

1. Delete config file:
```bash
rm shared/kick_config.json
```

2. Try to start stream via dashboard

**Expected:**
- Error message: "Kick configuration not found..."
- Container doesn't start
- User prompted to configure settings

**6.2 Test invalid credentials**

1. Set invalid credentials in dashboard
2. Try to start stream

**Expected:**
- Container starts but ffmpeg fails
- Logs show connection errors
- Container retries with exponential backoff
- Health check eventually fails

**6.3 Test compositor down**

1. Stop compositor:
```bash
docker compose stop compositor
```

2. Start Kick stream

**Expected:**
- Container starts
- Logs show "Connection refused" or similar
- Retry logic engages
- When compositor restarts, stream resumes

### Phase 7: Stress and Edge Cases

**7.1 Rapid start/stop**

1. Start stream
2. Immediately stop
3. Immediately start again
4. Wait 5 seconds, stop
5. Repeat 3-4 times

**Expected:**
- All operations complete successfully
- No zombie processes
- Container state always accurate in UI

**7.2 Multiple simultaneous requests**

```bash
# In separate terminals
curl -X POST http://localhost:3000/api/kick/start &
curl -X POST http://localhost:3000/api/kick/start &
curl -X POST http://localhost:3000/api/kick/start &
```

**Expected:**
- All requests handled gracefully
- Container only starts once
- No race conditions

**7.3 Network interruption recovery**

1. Start streaming
2. Simulate network issue:
```bash
docker compose exec ffmpeg-kick sh -c "pkill ffmpeg"
```

**Expected:**
- Container detects failure
- Retry logic engages (1s → 2s → 4s...)
- Stream reconnects automatically
- Health check recovers

### Phase 8: Cleanup and Final Verification

**8.1 Stop all services**
```bash
docker compose down
```

**8.2 Restart and verify persistence**
```bash
docker compose up -d
```

**Expected:**
- Configuration persists in shared/kick_config.json
- Dashboard loads saved credentials
- Can stream immediately without reconfiguration

## Success Criteria

✅ All containers build successfully
✅ Configuration UI works (save/load/toggle visibility)
✅ Backend API endpoints respond correctly
✅ Container starts/stops via dashboard
✅ Stream successfully reaches Kick.com
✅ Health checks pass
✅ Logs show no sensitive data
✅ Error handling works properly
✅ Simplified and Full views both work
✅ Configuration persists across restarts

## Common Issues and Solutions

### Issue: "Config file not found"
**Solution:** Configure via dashboard Kick Settings or create manually

### Issue: Container starts but immediately exits
**Solution:** 
- Check credentials are valid
- Verify compositor is running and accessible
- Check logs: `docker compose logs ffmpeg-kick`

### Issue: Stream doesn't appear on Kick
**Solution:**
- Verify credentials match your Kick dashboard
- Check stream key hasn't expired
- Ensure RTMPS URL is correct for your region
- Test network connectivity to Kick servers

### Issue: Health check failing
**Solution:**
- Check ffmpeg process: `docker compose exec ffmpeg-kick pgrep ffmpeg`
- Review logs for errors
- Verify TCP connection to compositor

## Rollback Procedure

If issues occur, revert by:

1. Stop the container:
```bash
docker compose stop ffmpeg-kick
```

2. Remove config (forces fallback to .env):
```bash
rm shared/kick_config.json
```

3. Use environment variables in .env instead
```bash
# Edit .env to have:
KICK_URL=your_url
KICK_KEY=your_key
```

## Next Steps After Testing

Once all tests pass:
- Document any discovered edge cases
- Update user documentation
- Train team on new features
- Monitor production logs for first week
- Set up alerting for stream health