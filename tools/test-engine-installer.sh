#!/usr/bin/env bash
# Offline tests: never select a device or call ADB.
set -euo pipefail
test_python="${1:-python3}"
source "$(dirname "${BASH_SOURCE[0]}")/build-and-install.sh"
PYTHON_EXE="$test_python"
BUILD_ROOT="$(mktemp -d)"
PLAN_ROOT="$BUILD_ROOT/plan"
for package in com.rockstargames.rdr com.netflix.NGP.Kamo; do
  for variant in 1 2; do
    "$PYTHON_EXE" - "$BUILD_ROOT" "$package" "$variant" <<'PY'
import hashlib,json,pathlib,sys
root=pathlib.Path(sys.argv[1]);out=root/'out';out.mkdir(exist_ok=True)
payload=b'offline fixture';(out/'base.apk').write_bytes(payload)
hashes=['3f80845edec32a663c42a8570e3131446813e695076c4bc96e2142c190bc22f3','0f288e2f9255d04277190bb5ffca742ec395d33a47fb305ddb22b9f0e904ef72']
data=dict(formatVersion=1,modVersion='0.1.1',package=sys.argv[2],versionCode='63226194',versionName='1.58.63226194',
    engineSha256=hashes[int(sys.argv[3])-1],engineVariant='variant-'+sys.argv[3],trade=0,buildId='a'*64,
    sourceSignerSha256='b'*64,outputSignerSha256='c'*64,outputSignerHashCode='12345678',
    outputs=[dict(file='base.apk',split=None,size=len(payload),sha256=hashlib.sha256(payload).hexdigest())])
(root/'build-manifest.json').write_text(json.dumps(data))
PY
    verify_build_manifest "$BUILD_ROOT/build-manifest.json"
    [ "$PACKAGE_NAME" = "$package" ]
    [ "$REMOTE_FILES_ROOT" = "/sdcard/Android/data/$package/files" ]
    assert_safe_generated_path "$REMOTE_FILES_ROOT/.rdrvr-backup-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    if [ "$package" = com.netflix.NGP.Kamo ]; then other=com.rockstargames.rdr; else other=com.netflix.NGP.Kamo; fi
    if (assert_safe_generated_path "/sdcard/Android/data/$other/files/.rdrvr-backup-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") >/dev/null 2>&1; then
      die "cross-package cleanup accepted"
    fi
  done
done
"$PYTHON_EXE" - "$BUILD_ROOT/build-manifest.json" <<'PY'
import json,pathlib,sys
p=pathlib.Path(sys.argv[1]);d=json.loads(p.read_text());d['engineSha256']='d'*64;p.write_text(json.dumps(d))
PY
if (verify_build_manifest "$BUILD_ROOT/build-manifest.json") >"$BUILD_ROOT/unknown.txt" 2>&1; then
  die "unknown engine accepted"
fi
grep -q 'report it to the author' "$BUILD_ROOT/unknown.txt"
echo "PASS: both engine/package combinations, unknown engine refusal and package-isolated paths. Fixtures: $BUILD_ROOT"
