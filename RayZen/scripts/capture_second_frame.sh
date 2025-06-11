#!/bin/bash
set -e

# Run RayZen and capture the second frame
# This assumes RayZen can save a frame as an image (e.g., via CLI or by default)
# If not, you may need to add this feature to your codebase.

# Example: run the app, let it render two frames, and save framebuffer
# You may need to modify RayZen to support this, e.g., via a --screenshot argument

../build/RayZen --screenshot=second_frame.png --screenshot-frame=2 || {
  echo "RayZen must support --screenshot and --screenshot-frame CLI options!";
  exit 1;
}

# If RayZen does not support this, you will need to implement framebuffer dump logic in your app.
