#!/usr/bin/env python3
"""
Privacy Manager for Compositor
Handles privacy mode state persistence and logic.
"""
import json
import os
from config import PRIVACY_STATE_FILE, STATE_SRT_CONNECTED


class PrivacyManager:
    """Manages privacy mode state and persistence."""
    
    def __init__(self, state_manager, switch_callback):
        """
        Initialize privacy manager.
        
        Args:
            state_manager: StateManager instance for state queries
            switch_callback: Callback function to switch away from SRT (e.g., to fallback)
        """
        self.state_manager = state_manager
        self.switch_callback = switch_callback
        self.privacy_enabled = False
        self._load_privacy_state()
    
    def _load_privacy_state(self):
        """Load privacy state from JSON file."""
        try:
            if os.path.exists(PRIVACY_STATE_FILE):
                with open(PRIVACY_STATE_FILE, 'r') as f:
                    data = json.load(f)
                    self.privacy_enabled = data.get('enabled', False)
                    print(f"[privacy] Loaded privacy state: enabled={self.privacy_enabled}", flush=True)
            else:
                print("[privacy] No saved privacy state found, starting with privacy disabled", flush=True)
        except Exception as e:
            print(f"[privacy] Error loading privacy state: {e}", flush=True)
            self.privacy_enabled = False
    
    def _save_privacy_state(self):
        """Save privacy state to JSON file."""
        try:
            with open(PRIVACY_STATE_FILE, 'w') as f:
                json.dump({'enabled': self.privacy_enabled}, f)
            print(f"[privacy] Saved privacy state: enabled={self.privacy_enabled}", flush=True)
        except Exception as e:
            print(f"[privacy] Error saving privacy state: {e}", flush=True)
    
    def set_privacy_mode(self, enabled):
        """
        Enable or disable privacy mode.
        
        Args:
            enabled: Boolean to enable/disable privacy mode
        """
        self.privacy_enabled = enabled
        self._save_privacy_state()
        
        if enabled:
            print("[privacy] Privacy mode ENABLED - SRT camera will not be shown", flush=True)
            # If SRT is currently active, switch away from it
            if self.state_manager.state == STATE_SRT_CONNECTED:
                print("[privacy] Switching away from SRT due to privacy mode activation", flush=True)
                self.switch_callback()
        else:
            print("[privacy] Privacy mode DISABLED - normal camera operation resumed", flush=True)
            # Note: The compositor will handle switching to SRT if it's connected
    
    def is_enabled(self):
        """Check if privacy mode is enabled."""
        return self.privacy_enabled
    
    def allows_srt(self):
        """Check if SRT can be shown (privacy mode is disabled)."""
        return not self.privacy_enabled