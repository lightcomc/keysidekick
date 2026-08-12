#!/usr/bin/env bash
# http_integration_tests.sh — end-to-end tests through running sidekick.exe HTTP API
# Tests: CSRF enforcement, CRUD lifecycle, profile switch, state consistency
# Requires: sidekick.exe running on 127.0.0.1:8765

BASE="http://127.0.0.1:8765"
CFG="$(cd "$(dirname "$0")/.." && pwd)/src/config.ini"

# The suite md5-sums and greps src/config.ini — fail fast if it is missing
# (CI copies config.example.ini → config.ini before running).
if [ ! -f "$CFG" ]; then
    echo "ERROR: $CFG not found — copy config.example.ini to config.ini first." >&2
    exit 1
fi

# Config safety: the suite mutates src/config.ini (combo macro, preset
# profiles, device activation). Snapshot it now and restore byte-for-byte
# on EXIT so developers and CI always get their config back.
CFG_BAK="$CFG.integration_bak"
cp "$CFG" "$CFG_BAK"
restore_config() {
    cp "$CFG_BAK" "$CFG"
    rm -f "$CFG_BAK"
}
trap restore_config EXIT

passed=0
failed=0

assert() {
    local name="$1" condition="$2"
    if eval "$condition" >/dev/null 2>&1; then
        echo "  ✓ $name"
        passed=$((passed+1))
    else
        echo "  ✗ $name"
        failed=$((failed+1))
    fi
}

echo "=== HTTP Integration Tests ==="
echo ""

# Check sidekick is running
if ! curl -s -o /dev/null "$BASE/api/status"; then
    echo "SIDEKICK NOT RUNNING. Start it first: ./src/sidekick.exe"
    exit 1
fi

# Get CSRF token from dashboard HTML
TOKEN=$(curl -s "$BASE/" | grep -o 'const CSRF_TOKEN="[^"]*"' | grep -o '"[^"]*"' | tail -1 | tr -d '"')

echo "--- Read endpoints ---"
STATUS=$(curl -s "$BASE/api/status")
assert "status returns device field" "echo '$STATUS' | grep -q '\"device\"'"
assert "status returns active field" "echo '$STATUS' | grep -q '\"active\"'"

PROFILES=$(curl -s "$BASE/api/profiles")
assert "profiles returns JSON array" "echo '$PROFILES' | grep -q '\"profiles\"'"

STATE=$(curl -s "$BASE/api/v1/state")
assert "state returns revision"      "echo '$STATE' | grep -q '\"revision\"'"
assert "state returns profileCount"  "echo '$STATE' | grep -q '\"profileCount\"'"

DIAG=$(curl -s "$BASE/api/v1/diagnostics")
assert "diagnostics returns device"     "echo '$DIAG' | grep -q '\"device\"'"
assert "diagnostics returns configExists" "echo '$DIAG' | grep -q '\"configExists\"'"

APPS=$(curl -s "$BASE/api/v1/applications")
assert "applications returns list" "echo '$APPS' | grep -q '\"applications\"'"

echo "--- HID-first onboarding endpoints ---"
HID=$(curl -s "$BASE/api/v1/hid")
assert "hid returns devices array"   "echo '$HID' | grep -q '\"devices\"'"
assert "hid device has status"       "echo '$HID' | grep -q '\"status\":'"
assert "hid device has service"      "echo '$HID' | grep -q '\"service\":'"
assert "hid device has kinds"        "echo '$HID' | grep -q '\"kinds\":'"

IDENT=$(curl -s "$BASE/api/v1/input/identify")
assert "input identify returns listening" "echo '$IDENT' | grep -q '\"listening\"'"

# Activate: invalid format → 400; a real (already-present) device → ok:true (dedupe)
BADACT=$(curl -s -o /dev/null -w "%{http_code}" -X POST "$BASE/api/v1/devices/activate" -H "Content-Type: application/json" -H "X-KeySidekick-Token: $TOKEN" -d '{"vidpid":"bogus"}')
assert "activate invalid vidpid → 400" "[ '$BADACT' = '400' ]"
ACTVID=$(echo "$HID" | grep -o '"vid":"[0-9a-f]\{4\}"' | head -1 | sed 's/.*:"//;s/"//')
ACTPID=$(echo "$HID" | grep -o '"pid":"[0-9a-f]\{4\}"' | head -1 | sed 's/.*:"//;s/"//')
if [ -n "$ACTVID" ] && [ -n "$ACTPID" ]; then
    ACTRES=$(curl -s -X POST "$BASE/api/v1/devices/activate" -H "Content-Type: application/json" -H "X-KeySidekick-Token: $TOKEN" -d "{\"vidpid\":\"vid_${ACTVID}&pid_${ACTPID}\"}")
    assert "activate real device → ok" "echo '$ACTRES' | grep -q '\"ok\":true'"
fi

echo "--- AI-agent presets + combo macros ---"
PRE=$(curl -s "$BASE/api/v1/presets")
assert "presets returns catalog" "echo '$PRE' | grep -q '\"presets\"'"
assert "presets lists codex agent" "echo '$PRE' | grep -q '\"codex\"'"
assert "presets lists devin agent" "echo '$PRE' | grep -q '\"devin\"'"
assert "presets lists reaper (DAW)" "echo '$PRE' | grep -q '\"reaper\"'"
assert "presets lists davinci + ableton + premiere + lightroom" "echo '$PRE' | grep -q '\"davinci\"' && echo '$PRE' | grep -q '\"ableton\"' && echo '$PRE' | grep -q '\"premiere\"' && echo '$PRE' | grep -q '\"lightroom\"'"

# Live click-to-fire: POST /api/v1/action/fire
FIRE=$(curl -s -X POST "$BASE/api/v1/action/fire" -H "Content-Type: application/json" -H "X-KeySidekick-Token: $TOKEN" -d '{"action":"{Media_Play_Pause}","usage":42,"profile":""}')
assert "action/fire ok → fired:true" "echo '$FIRE' | grep -q '\"fired\":true'"
FIRE_ACT=$(curl -s "$BASE/api/v1/activity" | grep -c 'Media_Play_Pause')
assert "action/fire recorded in activity" "[ '$FIRE_ACT' -ge 1 ]"
FIRE_EMPTY=$(curl -s -o /dev/null -w "%{http_code}" -X POST "$BASE/api/v1/action/fire" -H "Content-Type: application/json" -H "X-KeySidekick-Token: $TOKEN" -d '{"action":""}')
assert "action/fire empty action → 400" "[ '$FIRE_EMPTY' = '400' ]"
FIRE_NOBODY=$(curl -s -X POST "$BASE/api/v1/action/fire" -H "Content-Type: application/json" -H "X-KeySidekick-Token: $TOKEN" -d '{"action":"{F5}","profile":"no-such-profile"}')
assert "action/fire unknown profile → error" "echo '$FIRE_NOBODY' | grep -q 'not found'"
CFG_MD5_BEFORE=$(md5sum "$CFG" 2>/dev/null | cut -d' ' -f1)
curl -s -X POST "$BASE/api/v1/action/fire" -H "Content-Type: application/json" -H "X-KeySidekick-Token: $TOKEN" -d '{"action":"{F9}","profile":"basic"}' >/dev/null
sleep 1
CFG_MD5_AFTER=$(md5sum "$CFG" 2>/dev/null | cut -d' ' -f1)
assert "action/fire does not persist config" "[ '$CFG_MD5_BEFORE' = '$CFG_MD5_AFTER' ]"

ACT=$(curl -s "$BASE/api/v1/activity")
assert "activity returns events array" "echo '$ACT' | grep -q '\"events\"'"

EXP=$(curl -s "$BASE/api/v1/config/export")
assert "config export returns base64" "echo '$EXP' | grep -Eq '\"config\":\"[A-Za-z0-9+/=]{16,}\"'"

# Комбо-макрос {Ctrl+B} сохраняется (basic profile, как прочие тестовые маппинги)
CB=$(curl -s -o /dev/null -w "%{http_code}" -X POST "$BASE/api/key" -H "Content-Type: application/json" -H "X-KeySidekick-Token: $TOKEN" -d '{"profile":"basic","usage":201,"mod":0,"action":"{Ctrl+B}"}')
assert "combo macro accepted → 200" "[ '$CB' = '200' ]"
CBSTORE=$(curl -s "$BASE/api/profiles" | grep -Fc 'Ctrl+B')
assert "combo macro persisted" "[ '$CBSTORE' -ge 1 ]"

# Preset apply → profile with F1 mappings → delete → orphan application removed
AP2=$(curl -s -X POST "$BASE/api/v1/preset/apply" -H "Content-Type: application/json" -H "X-KeySidekick-Token: $TOKEN" -d '{"agentId":"claude","profileId":"test-claude","name":"ClaudeTest"}')
assert "preset apply (claude) → ok" "echo '$AP2' | grep -q '\"ok\":true'"
APKEYS=$(curl -s "$BASE/api/profiles" | grep -c '"usage":58')
assert "preset profile has F1 mapping" "[ '$APKEYS' -ge 1 ]"
sleep 1
ORPHAN_BEFORE=$(grep -c "app-test-claude\|ClaudeTest" "$CFG" 2>/dev/null)
assert "preset created linked app in config" "[ '$ORPHAN_BEFORE' -ge 1 ]"
DEL2=$(curl -s -X POST "$BASE/api/v1/profile/delete" -H "Content-Type: application/json" -H "X-KeySidekick-Token: $TOKEN" -d '{"id":"test-claude"}')
assert "preset profile delete → ok" "echo '$DEL2' | grep -q '\"ok\":true'"
sleep 1
ORPHAN_AFTER=$(grep -c "app-test-claude\|ClaudeTest" "$CFG" 2>/dev/null)
assert "orphan application cleaned on delete" "[ '$ORPHAN_AFTER' = '0' ]"

echo "--- CSRF enforcement ---"
# POST without token should 403
NOCRSF=$(curl -s -o /dev/null -w "%{http_code}" -X POST "$BASE/api/key" -H "Content-Type: application/json" -d '{"profile":"basic","usage":99,"mod":0,"action":"{F9}"}')
assert "POST without token → 403" "[ '$NOCRSF' = '403' ]"

# POST with wrong token should 403
WRONG=$(curl -s -o /dev/null -w "%{http_code}" -X POST "$BASE/api/key" -H "Content-Type: application/json" -H "X-KeySidekick-Token: wrongtoken" -d '{"profile":"basic","usage":99,"mod":0,"action":"{F9}"}')
assert "POST with wrong token → 403" "[ '$WRONG' = '403' ]"

# POST with correct token should succeed
OK=$(curl -s -o /dev/null -w "%{http_code}" -X POST "$BASE/api/key" -H "Content-Type: application/json" -H "X-KeySidekick-Token: $TOKEN" -d '{"profile":"basic","usage":99,"mod":0,"action":"{F9}"}')
assert "POST with correct token → 200" "[ '$OK' = '200' ]"

echo "--- Profile CRUD lifecycle ---"
# Create
CREATE=$(curl -s -X POST "$BASE/api/v1/profile/create" -H "Content-Type: application/json" -H "X-KeySidekick-Token: $TOKEN" -d '{"id":"test-integration","name":"TestIntegration","mode":"targeted"}')
assert "create profile" "echo '$CREATE' | grep -q '\"ok\":true'"

# Verify it exists
HAS=$(curl -s "$BASE/api/profiles" | grep -c "TestIntegration")
assert "created profile visible" "[ '$HAS' -ge 1 ]"

# Rename
RENAME=$(curl -s -X POST "$BASE/api/v1/profile/rename" -H "Content-Type: application/json" -H "X-KeySidekick-Token: $TOKEN" -d '{"id":"test-integration","newName":"RenamedTest"}')
assert "rename profile" "echo '$RENAME' | grep -q '\"ok\":true'"

# Duplicate
DUP=$(curl -s -X POST "$BASE/api/v1/profile/duplicate" -H "Content-Type: application/json" -H "X-KeySidekick-Token: $TOKEN" -d '{"sourceId":"test-integration","newId":"test-copy","newName":"TestCopy"}')
assert "duplicate profile" "echo '$DUP' | grep -q '\"ok\":true'"

# Delete both
DEL1=$(curl -s -X POST "$BASE/api/v1/profile/delete" -H "Content-Type: application/json" -H "X-KeySidekick-Token: $TOKEN" -d '{"id":"RenamedTest"}')
DEL2=$(curl -s -X POST "$BASE/api/v1/profile/delete" -H "Content-Type: application/json" -H "X-KeySidekick-Token: $TOKEN" -d '{"id":"test-copy"}')
assert "delete profile 1" "echo '$DEL1' | grep -q '\"ok\":true'"
assert "delete profile 2" "echo '$DEL2' | grep -q '\"ok\":true'"

# Verify they're gone
GONE=$(curl -s "$BASE/api/profiles" | grep -c "TestIntegration\|RenamedTest\|TestCopy")
assert "deleted profiles gone" "[ '$GONE' = '0' ]"

echo "--- State revision increments ---"
REV_BEFORE=$(curl -s "$BASE/api/v1/state" | grep -o '"revision":[0-9]*' | grep -o '[0-9]*')
# Детерминированный bump: создать профиль, активировать, удалить.
curl -s -X POST "$BASE/api/v1/profile/create" -H "Content-Type: application/json" -H "X-KeySidekick-Token: $TOKEN" -d '{"id":"revchk","name":"RevChk","mode":"basic"}' >/dev/null
curl -s -X POST "$BASE/api/profile/activate" -H "Content-Type: application/json" -H "X-KeySidekick-Token: $TOKEN" -d '{"name":"RevChk"}' >/dev/null
REV_AFTER=$(curl -s "$BASE/api/v1/state" | grep -o '"revision":[0-9]*' | grep -o '[0-9]*')
assert "revision incremented after create+switch" "[ '$REV_AFTER' -gt '$REV_BEFORE' ]"
curl -s -X POST "$BASE/api/v1/profile/delete" -H "Content-Type: application/json" -H "X-KeySidekick-Token: $TOKEN" -d '{"id":"revchk"}' >/dev/null

echo "--- Extended security + round-trip ---"

# Cross-origin POST with a valid token is still rejected by the Origin allow-list.
EVIL_ORIGIN=$(curl -s -o /dev/null -w "%{http_code}" -X POST "$BASE/api/profile/activate" -H "Content-Type: application/json" -H "X-KeySidekick-Token: $TOKEN" -H "Origin: http://evil.example" -d '{"name":"basic"}')
assert "activate with evil Origin → 403" "[ '$EVIL_ORIGIN' = '403' ]"

# Legacy mutating GET routes are gone: /switch and /profile now return 405.
GET_SWITCH=$(curl -s -o /dev/null -w "%{http_code}" "$BASE/switch")
assert "GET /switch → 405" "[ '$GET_SWITCH' = '405' ]"
GET_PROFILE=$(curl -s -o /dev/null -w "%{http_code}" "$BASE/profile")
assert "GET /profile → 405" "[ '$GET_PROFILE' = '405' ]"

# Non-JSON content type on a mutating endpoint → 415 Unsupported Media Type.
BAD_CT=$(curl -s -o /dev/null -w "%{http_code}" -X POST "$BASE/api/v1/applications/test-resolve" -H "Content-Type: text/plain" -H "X-KeySidekick-Token: $TOKEN" -d 'not-json')
assert "test-resolve with text/plain → 415" "[ '$BAD_CT' = '415' ]"

# Config export/import round-trip: export already returns base64; import it
# back verbatim and confirm a second export yields identical bytes.
EXPORT_1=$(curl -s "$BASE/api/v1/config/export")
CONFIG_B64=$(echo "$EXPORT_1" | grep -o '"config":"[^"]*"' | sed 's/^"config":"//;s/"$//')
IMPORT_RESP=$(curl -s -X POST "$BASE/api/v1/config/import" -H "Content-Type: application/json" -H "X-KeySidekick-Token: $TOKEN" -d "{\"config\":\"$CONFIG_B64\"}")
assert "config import → ok" "echo '$IMPORT_RESP' | grep -q '\"ok\":true'"
EXPORT_2=$(curl -s "$BASE/api/v1/config/export")
CONFIG_B64_2=$(echo "$EXPORT_2" | grep -o '"config":"[^"]*"' | sed 's/^"config":"//;s/"$//')
assert "config export/import round-trip equal" "[ '$CONFIG_B64' = '$CONFIG_B64_2' ]"

# Startup toggle accepts quoted-string booleans (JsonGetBool); enable then
# disable again so no autostart shortcut is left behind on the machine.
STARTUP_ON=$(curl -s -X POST "$BASE/api/v1/startup" -H "Content-Type: application/json" -H "X-KeySidekick-Token: $TOKEN" -d '{"enabled":"true"}')
assert "startup enable → ok:true installed:true" "echo '$STARTUP_ON' | grep -q '\"ok\":true' && echo '$STARTUP_ON' | grep -q '\"installed\":true'"
STARTUP_OFF=$(curl -s -X POST "$BASE/api/v1/startup" -H "Content-Type: application/json" -H "X-KeySidekick-Token: $TOKEN" -d '{"enabled":"false"}')
assert "startup disable → ok:true installed:false" "echo '$STARTUP_OFF' | grep -q '\"ok\":true' && echo '$STARTUP_OFF' | grep -q '\"installed\":false'"

echo ""
echo "=== Results: $passed passed, $failed failed ==="
[ $failed -eq 0 ] && exit 0 || exit 1
