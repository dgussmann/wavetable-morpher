#!/bin/bash
# Installs Wavetable Morpher for the current user and clears the macOS download quarantine.
set -e
cd "$(dirname "$0")"

VST3_DIR="$HOME/Library/Audio/Plug-Ins/VST3"
AU_DIR="$HOME/Library/Audio/Plug-Ins/Components"
APP_DIR="$HOME/Applications"
mkdir -p "$VST3_DIR" "$AU_DIR" "$APP_DIR"

for NAME in "Wavetable Morpher" "Wavetable Morpher Link"; do
  # Replace (never overwrite in place: macOS rejects a changed signed file it has already seen)
  rm -rf "$VST3_DIR/$NAME.vst3" "$AU_DIR/$NAME.component" "$APP_DIR/$NAME.app"
  cp -R "$NAME.vst3" "$VST3_DIR/"
  cp -R "$NAME.component" "$AU_DIR/"
  cp -R "$NAME.app" "$APP_DIR/"
  xattr -dr com.apple.quarantine "$VST3_DIR/$NAME.vst3" "$AU_DIR/$NAME.component" "$APP_DIR/$NAME.app" 2>/dev/null || true
done

# Make Logic / GarageBand notice the new Audio Unit
killall -9 AudioComponentRegistrar 2>/dev/null || true

echo ""
echo "Installed Wavetable Morpher and Wavetable Morpher Link (VST3, AU and standalone app)."
echo ""
echo "Now rescan plug-ins in your DAW. You can close this window."
