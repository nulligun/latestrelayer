/**
 * Upload Processor Service
 * 
 * Handles background processing of uploaded files (images and videos)
 * with progress reporting via WebSocket.
 */

const { spawn } = require('child_process');
const EventEmitter = require('events');
const path = require('path');
const fs = require('fs').promises;

class UploadProcessor extends EventEmitter {
  constructor() {
    super();
    this.currentJob = null;
    this.processingQueue = [];
    this.SHARED_DIR = '/app/shared';
  }

  /**
   * Get current processing status
   */
  getStatus() {
    if (!this.currentJob) {
      return { status: 'idle', job: null };
    }
    return {
      status: 'processing',
      job: {
        uploadId: this.currentJob.uploadId,
        fileType: this.currentJob.fileType,
        progress: this.currentJob.progress,
        message: this.currentJob.message
      }
    };
  }

  /**
   * Process an uploaded image file
   * @param {string} uploadId - Unique identifier for this upload
   * @param {string} sourcePath - Path to the uploaded image
   */
  async processImage(uploadId, sourcePath) {
    const tsOutputPath = path.join(this.SHARED_DIR, 'static-image.ts');
    const thumbnailPath = path.join(this.SHARED_DIR, 'offline-thumbnail.png');

    this.currentJob = {
      uploadId,
      fileType: 'image',
      progress: 0,
      message: 'Starting image processing...',
      sourcePath,
      startTime: Date.now()
    };

    this._emitProgress();

    try {
      // Step 1: Copy image as thumbnail (quick operation)
      this.currentJob.progress = 5;
      this.currentJob.message = 'Creating thumbnail...';
      this._emitProgress();

      await fs.copyFile(sourcePath, thumbnailPath);
      console.log(`[uploadProcessor] Created thumbnail: ${thumbnailPath}`);

      this.currentJob.progress = 10;
      this.currentJob.message = 'Converting to MPEG-TS format...';
      this._emitProgress();

      // Step 2: Convert image to MPEG-TS using ffmpeg with progress
      await this._runFfmpegWithProgress([
        '-y',
        '-loop', '1',
        '-i', sourcePath,
        '-f', 'lavfi',
        '-i', 'anullsrc=channel_layout=stereo:sample_rate=48000',
        '-vf', 'scale=1280:720:force_original_aspect_ratio=decrease,pad=1280:720:(ow-iw)/2:(oh-ih)/2:black',
        '-c:v', 'libx264',
        '-preset', 'fast',
        '-b:v', '3M',
        '-maxrate', '3M',
        '-bufsize', '6M',
        '-g', '30',
        '-keyint_min', '30',
        '-sc_threshold', '0',
        '-r', '30',
        '-pix_fmt', 'yuv420p',
        '-c:a', 'aac',
        '-b:a', '128k',
        '-bsf:v', 'h264_mp4toannexb',
        '-f', 'mpegts',
        '-mpegts_flags', '+resend_headers',
        '-mpegts_service_id', '1',
        '-mpegts_pmt_start_pid', '256',
        '-mpegts_start_pid', '257',
        '-muxrate', '5000000',
        '-t', '30',
        tsOutputPath
      ], 30, 60); // 30 seconds actual, display as 60

      this.currentJob.progress = 100;
      this.currentJob.message = 'Image processing completed';
      this._emitProgress('completed');

      console.log(`[uploadProcessor] Image processing completed: ${tsOutputPath}`);
      return { success: true, tsPath: tsOutputPath, thumbnailPath };

    } catch (error) {
      console.error(`[uploadProcessor] Image processing error:`, error.message);
      this.currentJob.message = `Error: ${error.message}`;
      this._emitProgress('error', error.message);
      throw error;
    } finally {
      this.currentJob = null;
    }
  }

  /**
   * Process an uploaded video file
   * @param {string} uploadId - Unique identifier for this upload
   * @param {string} sourcePath - Path to the uploaded video
   */
  async processVideo(uploadId, sourcePath) {
    const thumbnailPath = path.join(this.SHARED_DIR, 'offline-thumbnail.png');
    const tsOutputPath = path.join(this.SHARED_DIR, 'video.ts');

    this.currentJob = {
      uploadId,
      fileType: 'video',
      progress: 0,
      message: 'Starting video processing...',
      sourcePath,
      startTime: Date.now()
    };

    this._emitProgress();

    try {
      // Step 1: Get video duration for progress calculation
      this.currentJob.progress = 2;
      this.currentJob.message = 'Analyzing video...';
      this._emitProgress();

      const duration = await this._getVideoDuration(sourcePath);
      console.log(`[uploadProcessor] Video duration: ${duration}s`);

      // Step 2: Extract thumbnail from middle of video
      this.currentJob.progress = 5;
      this.currentJob.message = 'Extracting thumbnail...';
      this._emitProgress();

      const seekTime = duration / 2;
      await this._extractThumbnail(sourcePath, thumbnailPath, seekTime);
      console.log(`[uploadProcessor] Extracted thumbnail at ${seekTime.toFixed(2)}s`);

      // Step 3: Convert video to MPEG-TS with progress
      this.currentJob.progress = 10;
      this.currentJob.message = 'Converting to MPEG-TS format...';
      this._emitProgress();

      await this._runFfmpegWithProgress([
        '-y',
        '-i', sourcePath,
        '-vf', 'scale=1280:720:force_original_aspect_ratio=decrease,pad=1280:720,(ow-iw)/2:(oh-ih)/2:black',
        '-c:v', 'libx264',
        '-preset', 'fast',
        '-b:v', '3M',
        '-maxrate', '3M',
        '-bufsize', '6M',
        '-g', '30',
        '-keyint_min', '30',
        '-sc_threshold', '0',
        '-c:a', 'aac',
        '-b:a', '128k',
        '-bsf:v', 'h264_mp4toannexb',
        '-f', 'mpegts',
        '-mpegts_flags', '+resend_headers',
        '-mpegts_service_id', '1',
        '-mpegts_pmt_start_pid', '256',
        '-mpegts_start_pid', '257',
        '-muxrate', '5000000',
        tsOutputPath
      ], duration, duration * 2);

      this.currentJob.progress = 100;
      this.currentJob.message = 'Video processing completed';
      this._emitProgress('completed');

      console.log(`[uploadProcessor] Video processing completed: ${tsOutputPath}`);
      return { success: true, tsPath: tsOutputPath, thumbnailPath };

    } catch (error) {
      console.error(`[uploadProcessor] Video processing error:`, error.message);
      this.currentJob.message = `Error: ${error.message}`;
      this._emitProgress('error', error.message);
      throw error;
    } finally {
      this.currentJob = null;
    }
  }

  /**
   * Get video duration using ffprobe
   */
  async _getVideoDuration(videoPath) {
    return new Promise((resolve, reject) => {
      const ffprobe = spawn('ffprobe', [
        '-v', 'error',
        '-show_entries', 'format=duration',
        '-of', 'default=noprint_wrappers=1:nokey=1',
        videoPath
      ]);

      let output = '';
      let errorOutput = '';

      ffprobe.stdout.on('data', (data) => {
        output += data.toString();
      });

      ffprobe.stderr.on('data', (data) => {
        errorOutput += data.toString();
      });

      ffprobe.on('close', (code) => {
        if (code !== 0) {
          reject(new Error(`ffprobe failed: ${errorOutput}`));
          return;
        }
        const duration = parseFloat(output.trim());
        if (isNaN(duration) || duration <= 0) {
          reject(new Error('Could not determine video duration'));
          return;
        }
        resolve(duration);
      });

      ffprobe.on('error', (err) => {
        reject(new Error(`ffprobe spawn error: ${err.message}`));
      });
    });
  }

  /**
   * Extract a thumbnail from video at specified time
   */
  async _extractThumbnail(videoPath, thumbnailPath, seekTime) {
    return new Promise((resolve, reject) => {
      const ffmpeg = spawn('ffmpeg', [
        '-ss', seekTime.toString(),
        '-i', videoPath,
        '-vframes', '1',
        '-y',
        thumbnailPath
      ]);

      let errorOutput = '';

      ffmpeg.stderr.on('data', (data) => {
        errorOutput += data.toString();
      });

      ffmpeg.on('close', (code) => {
        if (code !== 0) {
          reject(new Error(`Thumbnail extraction failed: ${errorOutput}`));
          return;
        }
        resolve();
      });

      ffmpeg.on('error', (err) => {
        reject(new Error(`ffmpeg spawn error: ${err.message}`));
      });
    });
  }

  /**
   * Run ffmpeg with progress monitoring
   * @param {Array} args - ffmpeg arguments
   * @param {number} totalDuration - Expected output duration in seconds
   * @param {number} displayDuration - Duration to display in progress message (optional, defaults to totalDuration)
   */
  async _runFfmpegWithProgress(args, totalDuration, displayDuration = null) {
    // Use displayDuration for message if provided, otherwise use totalDuration
    const durationForDisplay = displayDuration !== null ? displayDuration : totalDuration;
    
    return new Promise((resolve, reject) => {
      // Add progress output to pipe
      const ffmpegArgs = ['-progress', 'pipe:1', ...args];
      const ffmpeg = spawn('ffmpeg', ffmpegArgs);

      let stderrOutput = '';

      // Parse progress from stdout (when using -progress pipe:1)
      ffmpeg.stdout.on('data', (data) => {
        const lines = data.toString().split('\n');
        for (const line of lines) {
          // Parse out_time_ms or out_time
          if (line.startsWith('out_time_ms=')) {
            const timeMs = parseInt(line.split('=')[1], 10);
            if (!isNaN(timeMs) && totalDuration > 0) {
              const currentTime = timeMs / 1000000; // Convert microseconds to seconds
              // Progress from 10% to 100% based on encoding time
              const progressPercent = Math.min(10 + (currentTime / totalDuration) * 90, 99);
              
              if (this.currentJob && progressPercent > this.currentJob.progress) {
                this.currentJob.progress = Math.round(progressPercent);
                this.currentJob.message = `Converting... ${Math.round(currentTime)}s / ${Math.round(durationForDisplay)}s`;
                this._emitProgress();
              }
            }
          } else if (line.startsWith('out_time=')) {
            // Alternative: parse HH:MM:SS.mmm format
            const timeStr = line.split('=')[1];
            const currentTime = this._parseTimeString(timeStr);
            if (currentTime > 0 && totalDuration > 0) {
              const progressPercent = Math.min(10 + (currentTime / totalDuration) * 90, 99);
              
              if (this.currentJob && progressPercent > this.currentJob.progress) {
                this.currentJob.progress = Math.round(progressPercent);
                this.currentJob.message = `Converting... ${Math.round(currentTime)}s / ${Math.round(durationForDisplay)}s`;
                this._emitProgress();
              }
            }
          }
        }
      });

      ffmpeg.stderr.on('data', (data) => {
        stderrOutput += data.toString();
      });

      ffmpeg.on('close', (code) => {
        if (code !== 0) {
          reject(new Error(`ffmpeg exited with code ${code}: ${stderrOutput.slice(-500)}`));
          return;
        }
        resolve();
      });

      ffmpeg.on('error', (err) => {
        reject(new Error(`ffmpeg spawn error: ${err.message}`));
      });
    });
  }

  /**
   * Parse ffmpeg time string (HH:MM:SS.mmm) to seconds
   */
  _parseTimeString(timeStr) {
    if (!timeStr || timeStr === 'N/A') return 0;
    
    const parts = timeStr.trim().split(':');
    if (parts.length !== 3) return 0;
    
    const hours = parseFloat(parts[0]) || 0;
    const minutes = parseFloat(parts[1]) || 0;
    const seconds = parseFloat(parts[2]) || 0;
    
    return hours * 3600 + minutes * 60 + seconds;
  }

  /**
   * Emit progress event
   */
  _emitProgress(status = 'processing', error = null) {
    if (!this.currentJob) return;

    const progressData = {
      type: 'upload_progress',
      uploadId: this.currentJob.uploadId,
      fileType: this.currentJob.fileType,
      status: status,
      progress: this.currentJob.progress,
      message: this.currentJob.message,
      error: error
    };

    this.emit('progress', progressData);
  }
}

// Export singleton instance
module.exports = new UploadProcessor();