#!/bin/bash
# Installs Wavetable Morpher for the current user and clears the macOS download quarantine.
set -e
cd "$(dirname "$0")"

VST3_DIR="$HOME/Library/Audio/Plug-Ins/VST3"
AU_DIR="$HOME/Library/Audio/Plug-Ins/Components"
APP_DIR="$HOME/Applications"
mkdir -p "$VST3_DIR" "$AU_DIR" "$APP_DIR"

rm -rf "$VST3_DIR/Wavetable Morpher.vst3" "$AU_DIR/Wavetable Morpher.component" "$APP_DIR/Wavetable Morpher.app"
cp -R "Wavetable Morpher.vst3" "$VST3_DIR/"
cp -R "Wavetable Morpher.component" "$AU_DIR/"
cp -R "Wavetable Morpher.app" "$APP_DIR/"

xattr -dr com.apple.quarantine "$VST3_DIR/Wavetable Morpher.vst3" "$AU_DIR/Wavetable Morpher.component" "$APP_DIR/Wavetable Morpher.app" 2>/dev/null || true

# Make Logic / GarageBand notice the new Audio Unit
killall -9 AudioComponentRegistrar 2>/dev/null || true

echo ""
echo "Installed:"
echo "  VST3:       $VST3_DIR/Wavetable Morpher.vst3"
echo "  AU:         $AU_DIR/Wavetable Morpher.component"
echo "  Standalone: $APP_DIR/Wavetable Morpher.app"
echo ""
echo "Now rescan plug-ins in your DAW. You can close this window."
