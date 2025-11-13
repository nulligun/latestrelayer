<template>
  <transition name="modal-fade">
    <div v-if="isVisible" class="modal-backdrop" @click.self="onCancel">
      <div class="modal-container">
        <div class="modal-header">
          <h2>{{ title }}</h2>
        </div>
        <div class="modal-body">
          <p>{{ message }}</p>
        </div>
        <div class="modal-footer">
          <button 
            class="modal-button button-cancel" 
            @click="onCancel"
            :disabled="isProcessing"
          >
            No
          </button>
          <button 
            class="modal-button button-confirm" 
            @click="onConfirm"
            :disabled="isProcessing"
          >
            Yes
          </button>
        </div>
      </div>
    </div>
  </transition>
</template>

<script>
export default {
  name: 'ConfirmationModal',
  props: {
    isVisible: {
      type: Boolean,
      default: false
    },
    title: {
      type: String,
      required: true
    },
    message: {
      type: String,
      required: true
    },
    isProcessing: {
      type: Boolean,
      default: false
    }
  },
  emits: ['confirm', 'cancel'],
  methods: {
    onConfirm() {
      if (!this.isProcessing) {
        this.$emit('confirm');
      }
    },
    onCancel() {
      if (!this.isProcessing) {
        this.$emit('cancel');
      }
    }
  }
};
</script>

<style scoped>
.modal-fade-enter-active,
.modal-fade-leave-active {
  transition: opacity 0.3s ease;
}

.modal-fade-enter-from,
.modal-fade-leave-to {
  opacity: 0;
}

.modal-backdrop {
  position: fixed;
  top: 0;
  left: 0;
  width: 100%;
  height: 100%;
  background: rgba(0, 0, 0, 0.7);
  display: flex;
  justify-content: center;
  align-items: center;
  z-index: 9999;
  backdrop-filter: blur(4px);
}

.modal-container {
  background: #1e293b;
  border-radius: 12px;
  padding: 0;
  max-width: 500px;
  width: 90%;
  box-shadow: 0 20px 60px rgba(0, 0, 0, 0.5);
  border: 1px solid #334155;
  animation: modal-appear 0.3s ease;
}

@keyframes modal-appear {
  from {
    transform: scale(0.9) translateY(-20px);
    opacity: 0;
  }
  to {
    transform: scale(1) translateY(0);
    opacity: 1;
  }
}

.modal-header {
  padding: 24px 24px 16px 24px;
  border-bottom: 1px solid #334155;
}

.modal-header h2 {
  margin: 0;
  font-size: 1.5rem;
  color: #f1f5f9;
  font-weight: 600;
}

.modal-body {
  padding: 24px;
}

.modal-body p {
  margin: 0;
  font-size: 1.1rem;
  color: #e2e8f0;
  line-height: 1.6;
}

.modal-footer {
  padding: 16px 24px 24px 24px;
  display: flex;
  gap: 12px;
  justify-content: flex-end;
}

.modal-button {
  padding: 12px 32px;
  font-size: 1rem;
  font-weight: 600;
  border: none;
  border-radius: 8px;
  cursor: pointer;
  transition: all 0.2s ease;
  min-width: 100px;
}

.modal-button:disabled {
  opacity: 0.5;
  cursor: not-allowed;
}

.button-cancel {
  background: #475569;
  color: #e2e8f0;
}

.button-cancel:hover:not(:disabled) {
  background: #64748b;
}

.button-confirm {
  background: #ef4444;
  color: white;
}

.button-confirm:hover:not(:disabled) {
  background: #dc2626;
}
</style>