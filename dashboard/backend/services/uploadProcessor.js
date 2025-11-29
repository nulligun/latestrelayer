/**
 * Upload Processor Service
 *
 * Handles background processing of uploaded files (images and videos)
 * with progress reporting via WebSocket.
 *
 * Encoding settings are read from environment variables:
 *   - VIDEO_WIDTH (default: 1280)
 *   - VIDEO_HEIGHT (default: 720)
 *   - VIDEO_FPS (default: 30)
 *   - VIDEO_ENCODER (default: libx264)
 *   - AUDIO_ENCODER (default: aac)
 *   - AUDIO_BITRATE (default: 128) in kbps
 *   - AUDIO_SAMPLE_RATE (default: 48000) in Hz
 *
 * Audio is always stereo (2 channels).
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
    
    // Read encoding settings from environment with defaults
    this.videoWidth = parseInt(process.env.VIDEO_WIDTH, 10) || 1280;
    this.videoHeight = parseInt(process.env.VIDEO_HEIGHT, 10) || 720;
    this.videoFps = parseInt(process.env.VIDEO_FPS, 10) || 30;
    this.videoEncoder = process.env.VIDEO_ENCODER || 'libx264';
    this.audioEncoder = process.env.AUDIO_ENCODER || 'aac';
    this.audioBitrate = parseInt(process.env.AUDIO_BITRATE, 10) || 128;
    this.audioSampleRate = parseInt(process.env.AUDIO_SAMPLE_RATE, 10) || 48000;
    
    console.log(`[uploadProcessor] Encoding settings:`);
    console.log(`[uploadProcessor]   Video: ${this.videoWidth}x${this.videoHeight} @ ${this.videoFps}fps (${this.videoEncoder})`);
    console.log(`[uploadProcessor]   Audio: ${this.audioEncoder} @ ${this.audioBitrate}kbps, ${this.audioSampleRate}Hz stereo`);
  }

  /**
   * Get current encoding settings
   * @returns {Object} Current encoding settings
   */
  getEncodingSettings() {
    return {
      videoWidth: this.videoWidth,
      videoHeight: this.videoHeight,
      videoFps: this.videoFps,
      videoEncoder: this.videoEncoder,
      audioEncoder: this.audioEncoder,
      audioBitrate: this.audioBitrate,
      audioSampleRate: this.audioSampleRate
    };
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
      // Using encoding settings from environment variables
      await this._runFfmpegWithProgress([
        '-y',
        '-loop', '1',
        '-i', sourcePath,
        '-f', 'lavfi',
        '-i', `anullsrc=channel_layout=stereo:sample_rate=${this.audioSampleRate}`,
        '-vf', `scale=${this.videoWidth}:${this.videoHeight}:force_original_aspect_ratio=decrease,pad=${this.videoWidth}:${this.videoHeight}:(ow-iw)/2:(oh-ih)/2:black`,
        '-c:v', this.videoEncoder,
        '-preset', 'fast',
        '-crf', '23',
        '-g', String(this.videoFps),
        '-keyint_min', String(this.videoFps),
        '-sc_threshold', '0',
        '-r', String(this.videoFps),
        '-pix_fmt', 'yuv420p',
        '-c:a', this.audioEncoder,
        '-b:a', `${this.audioBitrate}k`,
        '-bsf:v', 'h264_mp4toannexb',
        '-f', 'mpegts',
        '-mpegts_flags', '+resend_headers',
        '-mpegts_service_id', '1',
        '-mpegts_pmt_start_pid', '256',
        '-mpegts_start_pid', '257',
        '-muxrate', '10000000',
        '-t', '30',
        tsOutputPath
      ], 30); // 30 seconds of output

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
      // Using encoding settings from environment variables
      this.currentJob.progress = 10;
      this.currentJob.message = 'Converting to MPEG-TS format...';
      this._emitProgress();

      await this._runFfmpegWithProgress([
        '-y',
        '-i', sourcePath,
        '-vf', `scale=${this.videoWidth}:${this.videoHeight}:force_original_aspect_ratio=decrease,pad=${this.videoWidth}:${this.videoHeight}:(ow-iw)/2:(oh-ih)/2:black`,
        '-c:v', this.videoEncoder,
        '-preset', 'fast',
        '-crf', '23',
        '-g', String(this.videoFps),
        '-keyint_min', String(this.videoFps),
        '-sc_threshold', '0',
        '-c:a', this.audioEncoder,
        '-b:a', `${this.audioBitrate}k`,
        '-bsf:v', 'h264_mp4toannexb',
        '-f', 'mpegts',
        '-mpegts_flags', '+resend_headers',
        '-mpegts_service_id', '1',
        '-mpegts_pmt_start_pid', '256',
        '-mpegts_start_pid', '257',
        '-muxrate', '10000000',
        tsOutputPath
      ], duration);

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
   */
  async _runFfmpegWithProgress(args, totalDuration) {
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
                this.currentJob.message = `Converting... ${Math.round(currentTime)}s / ${Math.round(totalDuration)}s`;
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
                this.currentJob.message = `Converting... ${Math.round(currentTime)}s / ${Math.round(totalDuration)}s`;
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