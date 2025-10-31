import { createApp } from 'vue';
import App from './App.vue';

console.log('[main] Starting Stream Dashboard...');

const app = createApp(App);
app.mount('#app');

console.log('[main] Dashboard mounted');