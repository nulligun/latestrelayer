#!/usr/bin/env python3
"""
Relayer Main Entry Point
Starts the relayer manager and HTTP API server.
"""
import threading
from relayer_manager import RelayerManager
from http_api import run_http_server


def main():
    """Main entry point for relayer."""
    relayer = RelayerManager()
    
    # Start HTTP API server in background thread
    http_thread = threading.Thread(target=run_http_server, args=(relayer,), daemon=True)
    http_thread.start()
    
    # Start relayer pipeline and run main loop
    relayer.start_pipeline()
    relayer.run()


if __name__ == "__main__":
    main()