<template>
  <div class="kick-settings-card">
    <div class="header-row">
      <h2>Kick Settings</h2>
      <div v-if="configSource" class="source-badge" :class="sourceBadgeClass">
        {{ sourceBadgeText }}
      </div>
    </div>
    
    <div class="settings-content">
      <!-- Kick URL Field -->
      <div class="setting-field">
        <label class="setting-label">Kick Stream URL (RTMPS)</label>
        <div class="input-wrapper">
          <input
            v-model="kickUrl"
            :type="showUrl ? 'text' : 'password'"
            placeholder="rtmps://..."
            class="setting-input"
            :disabled="savingConfig"
            @focus="handleUrlFocus"
          />
          <button
            @click="toggleUrlVisibility"
            class="toggle-visibility-btn"
            :class="{ active: showUrl }"
            type="button"
          >
            {{ showUrl ? '👁️' : '👁️‍🗨️' }}
          </button>
        </div>
      </div>

      <!-- Kick Stream Key Field -->
      <div class="setting-field">
        <label class="setting-label">Kick Stream Key</label>
        <div class="input-wrapper">
          <input
            v-model="kickKey"
            :type="showKey ? 'text' : 'password'"
            placeholder="sk_..."
            class="setting-input"
            :disabled="savingConfig"
            @focus="handleKeyFocus"
          />
          <button
            @click="toggleKeyVisibility"
            class="toggle-visibility-btn"
            :class="{ active: showKey }"
            type="button"
          >
            {{ showKey ? '👁️' : '👁️‍🗨️' }}
          </button>
        </div>
      </div>

      <!-- Action Buttons -->
      <div class="button-row">
        <button
          @click="saveConfiguration"
          :disabled="savingConfig || !isConfigValid"
          class="save-button"
        >
          {{ savingConfig ? 'Saving...' : 'Save Configuration' }}
        </button>
        <button
          @click="showResetModal"
          :disabled="savingConfig || resettingConfig"
          class="reset-button"
        >
          {{ resettingConfig ? 'Resetting...' : 'Reset to Default' }}
        </button>
      </div>

      <!-- Status Messages -->
      <div v-if="saveSuccess" class="status-message success">
        ✓ Configuration saved successfully
      </div>
      <div v-if="resetSuccess" class="status-message success">
        ✓ Configuration reset to defaults from environment variables
      </div>
      <div v-if="saveError" class="status-message error">
        {{ saveError }}
      </div>
      
      <!-- Warning Message -->
      <div class="warning-message">
        ⚠️ <strong>Note:</strong> These credentials are sensitive. Keep them obscured when streaming to avoid accidental exposure.
      </div>
    </div>
    
    <!-- Confirmation Modal -->
    <ConfirmationModal
      :isVisible="showConfirmModal"
      title="Reset Kick Settings?"
      :message="confirmModalMessage"
      :isProcessing="resettingConfig"
      @confirm="confirmReset"
      @cancel="cancelReset"
    />
  </div>
</template>

<script>
import ConfirmationModal from './ConfirmationModal.vue';

export default {
  name: 'KickSettings',
  components: {
    ConfirmationModal
  },
  data() {
    return {
      kickUrl: '',
      kickKey: '',
      showUrl: false,
      showKey: false,
      savingConfig: false,
      saveSuccess: false,
      saveError: null,
      resettingConfig: false,
      resetSuccess: false,
      showConfirmModal: false,
      configSource: null
    };
  },
  computed: {
    isConfigValid() {
      return this.kickUrl.trim().length > 0 && this.kickKey.trim().length > 0;
    },
    sourceBadgeText() {
      return this.configSource === 'config' 
        ? 'Using saved config' 
        : 'Using defaults from .env';
    },
    sourceBadgeClass() {
      return this.configSource === 'config' ? 'source-config' : 'source-env';
    },
    confirmModalMessage() {
      return `This will:\n\n• Delete saved configuration\n• Reload defaults from environment variables\n• Restart ffmpeg-kick container if running\n\nYou can save new settings anytime.`;
    }
  },
  mounted() {
    this.loadConfiguration();
  },
  methods: {
    async loadConfiguration() {
      try {
        const response = await fetch('/api/kick/config');
        if (!response.ok) {
          throw new Error('Failed to load configuration');
        }
        
        const config = await response.json();
        this.kickUrl = config.kickUrl || '';
        this.kickKey = config.kickKey || '';
        this.configSource = config.source || 'env';
        
        console.log('[KickSettings] Configuration loaded from:', this.configSource);
      } catch (error) {
        console.error('[KickSettings] Error loading configuration:', error);
        this.saveError = `Failed to load configuration: ${error.message}`;
      }
    },
    
    async saveConfiguration() {
      if (!this.isConfigValid) {
        this.saveError = 'Please provide both URL and Stream Key';
        return;
      }
      
      this.savingConfig = true;
      this.saveSuccess = false;
      this.resetSuccess = false;
      this.saveError = null;
      
      try {
        const response = await fetch('/api/kick/config', {
          method: 'POST',
          headers: {
            'Content-Type': 'application/json'
          },
          body: JSON.stringify({
            kickUrl: this.kickUrl.trim(),
            kickKey: this.kickKey.trim()
          })
        });
        
        if (!response.ok) {
          const data = await response.json();
          throw new Error(data.error || 'Failed to save configuration');
        }
        
        const result = await response.json();
        console.log('[KickSettings] Configuration saved:', result);
        
        this.saveSuccess = true;
        this.configSource = 'config';
        
        // Hide success message after 3 seconds
        setTimeout(() => {
          this.saveSuccess = false;
        }, 3000);
        
        // Auto-hide credentials after saving for security
        this.showUrl = false;
        this.showKey = false;
        
      } catch (error) {
        console.error('[KickSettings] Error saving configuration:', error);
        this.saveError = error.message;
      } finally {
        this.savingConfig = false;
      }
    },
    
    showResetModal() {
      this.showConfirmModal = true;
    },
    
    cancelReset() {
      this.showConfirmModal = false;
    },
    
    async confirmReset() {
      this.resettingConfig = true;
      this.saveSuccess = false;
      this.resetSuccess = false;
      this.saveError = null;
      
      try {
        const response = await fetch('/api/kick/config', {
          method: 'DELETE'
        });
        
        if (!response.ok) {
          const data = await response.json();
          throw new Error(data.error || 'Failed to reset configuration');
        }
        
        const result = await response.json();
        console.log('[KickSettings] Configuration reset:', result);
        
        // Update UI with environment defaults
        this.kickUrl = result.config.kickUrl || '';
        this.kickKey = result.config.kickKey || '';
        this.configSource = 'env';
        
        this.resetSuccess = true;
        this.showConfirmModal = false;
        
        // Hide success message after 3 seconds
        setTimeout(() => {
          this.resetSuccess = false;
        }, 3000);
        
        // Auto-hide credentials for security
        this.showUrl = false;
        this.showKey = false;
        
      } catch (error) {
        console.error('[KickSettings] Error resetting configuration:', error);
        this.saveError = error.message;
        this.showConfirmModal = false;
      } finally {
        this.resettingConfig = false;
      }
    },
    
    toggleUrlVisibility() {
      this.showUrl = !this.showUrl;
    },
    
    toggleKeyVisibility() {
      this.showKey = !this.showKey;
    },
    
    handleUrlFocus() {
      // Optionally auto-reveal on focus
      // this.showUrl = true;
    },
    
    handleKeyFocus() {
      // Optionally auto-reveal on focus
      // this.showKey = true;
    }
  }
};
</script>

<style scoped>
.kick-settings-card {
  background: #1e293b;
  border-radius: 8px;
  padding: 20px;
  box-shadow: 0 4px 6px rgba(0, 0, 0, 0.1);
  margin-bottom: 20px;
}

.header-row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 20px;
  gap: 12px;
}

h2 {
  margin: 0;
  font-size: 1.25rem;
  color: #f1f5f9;
}

.source-badge {
  padding: 6px 12px;
  border-radius: 6px;
  font-size: 0.75rem;
  font-weight: 600;
  white-space: nowrap;
}

.source-badge.source-config {
  background: rgba(16, 185, 129, 0.15);
  color: #10b981;
  border: 1px solid #10b981;
}

.source-badge.source-env {
  background: rgba(59, 130, 246, 0.15);
  color: #3b82f6;
  border: 1px solid #3b82f6;
}

.settings-content {
  display: flex;
  flex-direction: column;
  gap: 20px;
}

.setting-field {
  display: flex;
  flex-direction: column;
  gap: 8px;
}

.setting-label {
  font-size: 0.875rem;
  color: #e2e8f0;
  font-weight: 500;
}

.input-wrapper {
  display: flex;
  gap: 8px;
  align-items: center;
}

.setting-input {
  flex: 1;
  padding: 10px 12px;
  background: #0f172a;
  border: 1px solid #334155;
  border-radius: 6px;
  color: #e2e8f0;
  font-size: 0.9rem;
  font-family: 'Monaco', 'Menlo', 'Ubuntu Mono', monospace;
  transition: border-color 0.2s ease;
}

.setting-input:hover:not(:disabled) {
  border-color: #3b82f6;
}

.setting-input:focus {
  outline: none;
  border-color: #3b82f6;
  box-shadow: 0 0 0 3px rgba(59, 130, 246, 0.1);
}

.setting-input:disabled {
  opacity: 0.5;
  cursor: not-allowed;
}

.setting-input::placeholder {
  color: #64748b;
}

.toggle-visibility-btn {
  padding: 10px 12px;
  background: #334155;
  border: 1px solid #475569;
  border-radius: 6px;
  color: #e2e8f0;
  font-size: 1.2rem;
  cursor: pointer;
  transition: all 0.2s ease;
  min-width: 44px;
  display: flex;
  align-items: center;
  justify-content: center;
}

.toggle-visibility-btn:hover {
  background: #475569;
  border-color: #64748b;
}

.toggle-visibility-btn.active {
  background: #3b82f6;
  border-color: #3b82f6;
}

.button-row {
  display: flex;
  gap: 10px;
  margin-top: 10px;
}

.save-button,
.reset-button {
  padding: 12px 24px;
  color: white;
  border: none;
  border-radius: 6px;
  font-size: 0.9rem;
  font-weight: 600;
  cursor: pointer;
  transition: all 0.2s ease;
}

.save-button {
  background: #3b82f6;
}

.save-button:hover:not(:disabled) {
  background: #2563eb;
  transform: translateY(-1px);
  box-shadow: 0 4px 8px rgba(59, 130, 246, 0.3);
}

.save-button:active:not(:disabled) {
  background: #1d4ed8;
  transform: translateY(0);
}

.reset-button {
  background: #64748b;
}

.reset-button:hover:not(:disabled) {
  background: #475569;
  transform: translateY(-1px);
  box-shadow: 0 4px 8px rgba(100, 116, 139, 0.3);
}

.reset-button:active:not(:disabled) {
  background: #334155;
  transform: translateY(0);
}

.save-button:disabled,
.reset-button:disabled {
  background: #475569;
  cursor: not-allowed;
  opacity: 0.6;
}

.status-message {
  padding: 12px;
  border-radius: 6px;
  font-size: 0.875rem;
  font-weight: 500;
}

.status-message.success {
  background: rgba(16, 185, 129, 0.1);
  color: #10b981;
  border: 1px solid #10b981;
}

.status-message.error {
  background: rgba(239, 68, 68, 0.1);
  color: #ef4444;
  border: 1px solid #ef4444;
}

.warning-message {
  padding: 12px;
  background: rgba(245, 158, 11, 0.1);
  border: 1px solid #f59e0b;
  border-radius: 6px;
  color: #f59e0b;
  font-size: 0.875rem;
  line-height: 1.5;
}

.warning-message strong {
  font-weight: 600;
}

@media (max-width: 768px) {
  .kick-settings-card {
    padding: 15px;
  }
  
  .header-row {
    flex-direction: column;
    align-items: flex-start;
  }
  
  .input-wrapper {
    flex-direction: column;
  }
  
  .toggle-visibility-btn {
    width: 100%;
  }
  
  .button-row {
    flex-direction: column;
  }
  
  .save-button,
  .reset-button {
    width: 100%;
  }
}
</style>