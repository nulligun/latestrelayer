<template>
  <div class="kick-settings-card">
    <div class="header-row">
      <h2>Kick Settings</h2>
    </div>
    
    <div class="settings-content">
      <!-- Kick URL Field -->
      <div class="setting-field">
        <label class="setting-label">Kick Stream URL (RTMPS)</label>
        <div class="input-wrapper">
          <input
            :value="displayUrlValue"
            @input="handleUrlInput"
            type="text"
            :placeholder="urlPlaceholder"
            class="setting-input"
            :disabled="savingConfig"
            @focus="handleUrlFocus"
          />
          <button
            @click="toggleUrlVisibility"
            class="toggle-visibility-btn"
            type="button"
            :title="showUrl ? 'Hide URL' : 'Show URL'"
          >
            {{ showUrl ? '🔓' : '🔒' }}
          </button>
        </div>
      </div>

      <!-- Kick Stream Key Field -->
      <div class="setting-field">
        <label class="setting-label">Kick Stream Key</label>
        <div class="input-wrapper">
          <input
            :value="displayKeyValue"
            @input="handleKeyInput"
            type="text"
            :placeholder="keyPlaceholder"
            class="setting-input"
            :disabled="savingConfig"
            @focus="handleKeyFocus"
          />
          <button
            @click="toggleKeyVisibility"
            class="toggle-visibility-btn"
            type="button"
            :title="showKey ? 'Hide Key' : 'Show Key'"
          >
            {{ showKey ? '🔓' : '🔒' }}
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
      envUrl: '',  // Store env defaults separately
      envKey: '',  // Store env defaults separately
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
      // For env source, check if user has entered values OR env values exist
      if (this.configSource === 'env') {
        return (this.kickUrl.trim().length > 0 || this.envUrl.length > 0) &&
               (this.kickKey.trim().length > 0 || this.envKey.length > 0);
      }
      return this.kickUrl.trim().length > 0 && this.kickKey.trim().length > 0;
    },
    urlPlaceholder() {
      if (this.configSource === 'env') {
        return this.showUrl ? this.envUrl : '••••••••••••••••••••••••••••••';
      }
      return 'rtmps://...';
    },
    keyPlaceholder() {
      if (this.configSource === 'env') {
        return this.showKey ? this.envKey : '••••••••••••••••••••••••••••••';
      }
      return 'sk_...';
    },
    displayUrlValue() {
      if (this.configSource === 'env') {
        // For env source, show user input (may be empty)
        return this.kickUrl;
      }
      // For saved config source
      if (this.showUrl) {
        return this.kickUrl;
      }
      // Show asterisks when locked (if there's a value)
      return this.kickUrl ? '••••••••••••••••••••••••••••••' : '';
    },
    displayKeyValue() {
      if (this.configSource === 'env') {
        // For env source, show user input (may be empty)
        return this.kickKey;
      }
      // For saved config source
      if (this.showKey) {
        return this.kickKey;
      }
      // Show asterisks when locked (if there's a value)
      return this.kickKey ? '••••••••••••••••••••••••••••••' : '';
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
        this.configSource = config.source || 'env';
        
        if (this.configSource === 'env') {
          // Store env values separately for placeholder display
          this.envUrl = config.kickUrl || '';
          this.envKey = config.kickKey || '';
          // Keep input fields empty for user input
          this.kickUrl = '';
          this.kickKey = '';
        } else {
          // For saved config, populate the input fields
          this.kickUrl = config.kickUrl || '';
          this.kickKey = config.kickKey || '';
          this.envUrl = '';
          this.envKey = '';
        }
        
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
      
      // Determine which values to save
      const urlToSave = this.kickUrl.trim() || this.envUrl;
      const keyToSave = this.kickKey.trim() || this.envKey;
      
      try {
        const response = await fetch('/api/kick/config', {
          method: 'POST',
          headers: {
            'Content-Type': 'application/json'
          },
          body: JSON.stringify({
            kickUrl: urlToSave,
            kickKey: keyToSave
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
        
        // Update kickUrl/kickKey with the saved values
        this.kickUrl = urlToSave;
        this.kickKey = keyToSave;
        this.envUrl = '';
        this.envKey = '';
        
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
        this.envUrl = result.config.kickUrl || '';
        this.envKey = result.config.kickKey || '';
        this.kickUrl = '';
        this.kickKey = '';
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
    
    handleUrlInput(event) {
      if (this.configSource === 'env') {
        // For env source, just update the user input
        this.kickUrl = event.target.value;
      } else {
        // For saved config, only update when unlocked
        if (this.showUrl) {
          this.kickUrl = event.target.value;
        }
      }
    },
    
    handleKeyInput(event) {
      if (this.configSource === 'env') {
        // For env source, just update the user input
        this.kickKey = event.target.value;
      } else {
        // For saved config, only update when unlocked
        if (this.showKey) {
          this.kickKey = event.target.value;
        }
      }
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
  background: transparent;
  border: none;
  color: #94a3b8;
  font-size: 1.1rem;
  cursor: pointer;
  padding: 8px;
  transition: color 0.2s ease, transform 0.1s ease;
  flex-shrink: 0;
}

.toggle-visibility-btn:hover {
  color: #e2e8f0;
  transform: scale(1.1);
}

.toggle-visibility-btn:active {
  transform: scale(0.95);
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