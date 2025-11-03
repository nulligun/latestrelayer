# Scene Switching Feature - Implementation Plan

## Problem
The `apply_diff` tool reported success but changes were NOT actually saved to source files. Need to rewrite files completely.

## Files That Need Complete Rewrite

### 1. stream-dashboard/frontend/src/App.vue
**Missing Code:**
- Line 102: `sceneDurationSeconds: 0` in data
- Line 111-122: Scene change detection in `handleMessage`
- Line 190-205: `handleSceneSwitching` function
- Line 196-199: Timeout cleanup in `onUnmounted`
- Line 206: `switchingScene` in return
- Line 208: `handleSceneSwitching` in return

### 2. stream-dashboard/frontend/src/components/StreamStats.vue  
**Missing Code:**
- Line 57: `:disabled="switching || switchingScene"` 
- Line 124-129: New diagnostic logs and event emission in `switchScene`

## Implementation Strategy
Use `write_to_file` to completely rewrite both files with ALL changes included, since `apply_diff` is not persisting changes correctly.

## Testing
After rewrite, rebuild container and verify:
1. Source files contain all changes
2. Build produces new JS hash
3. Features work in browser