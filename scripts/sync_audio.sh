#!/bin/bash
# sync_audio.sh — Pull ASR input and TTS output audio from HiLily server
# Usage: ./scripts/sync_audio.sh [asr|tts|all] [--latest N]
#
# Examples:
#   ./scripts/sync_audio.sh              # sync all audio (ASR + TTS)
#   ./scripts/sync_audio.sh asr          # sync ASR input only
#   ./scripts/sync_audio.sh tts          # sync TTS output only
#   ./scripts/sync_audio.sh all --latest 5  # sync latest 5 files of each type

SERVER="maxazure@192.168.31.205"
REMOTE_BASE="~/projects/hilily-server/audio_logs"
LOCAL_BASE="$(dirname "$0")/../debug_audio"

TYPE="${1:-all}"
LATEST=""

if [[ "$2" == "--latest" ]]; then
    LATEST="$3"
fi

mkdir -p "$LOCAL_BASE/asr" "$LOCAL_BASE/tts"

sync_dir() {
    local remote_dir="$1"
    local local_dir="$2"
    local label="$3"

    echo "🔄 Syncing $label audio..."

    if [[ -n "$LATEST" ]]; then
        # Get latest N files
        files=$(ssh "$SERVER" "ls -t $remote_dir/*.wav 2>/dev/null | head -$LATEST" 2>/dev/null)
        if [[ -z "$files" ]]; then
            echo "   No $label audio files found"
            return
        fi
        count=0
        while IFS= read -r f; do
            scp -q "$SERVER:$f" "$local_dir/" && ((count++))
        done <<< "$files"
        echo "   ✅ Synced $count $label files"
    else
        rsync -avz --progress "$SERVER:$remote_dir/*.wav" "$local_dir/" 2>/dev/null
        if [[ $? -eq 0 ]]; then
            echo "   ✅ $label sync complete"
        else
            echo "   ⚠️  No $label audio files or sync failed"
        fi
    fi

    local total
    total=$(ls -1 "$local_dir"/*.wav 2>/dev/null | wc -l | tr -d ' ')
    echo "   📁 Local $label files: $total"
}

case "$TYPE" in
    asr)
        sync_dir "$REMOTE_BASE" "$LOCAL_BASE/asr" "ASR (input)"
        ;;
    tts)
        sync_dir "$REMOTE_BASE/tts" "$LOCAL_BASE/tts" "TTS (output)"
        ;;
    all)
        sync_dir "$REMOTE_BASE" "$LOCAL_BASE/asr" "ASR (input)"
        echo ""
        sync_dir "$REMOTE_BASE/tts" "$LOCAL_BASE/tts" "TTS (output)"
        ;;
    *)
        echo "Usage: $0 [asr|tts|all] [--latest N]"
        exit 1
        ;;
esac

echo ""
echo "📂 Audio files saved to: $LOCAL_BASE"
echo "   Play with: afplay debug_audio/asr/<file>.wav"
