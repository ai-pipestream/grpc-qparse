#!/usr/bin/env bash
# Exercises scripts/publish-manifests.sh against a fake `docker` on PATH: no
# registry, no daemon. The fake records every invocation and answers
# `imagetools inspect --raw` with the index JSON the test chooses, so the
# cases below pin the tags the script creates, the sources it names, and the
# gates it refuses to pass (missing leg, missing platform, missing
# attestation). Exit code is the verdict.
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
script="$here/publish-manifests.sh"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

mkdir -p "$work/bin"
cat >"$work/bin/docker" <<'EOF'
#!/usr/bin/env bash
echo "$*" >>"$FAKE_DOCKER_LOG"
if [[ "$1 $2 $3 $4" == "buildx imagetools inspect --raw" ]]; then
  cat "$FAKE_INDEX_JSON"
fi
EOF
chmod +x "$work/bin/docker"
export PATH="$work/bin:$PATH"
export FAKE_DOCKER_LOG="$work/docker.log"
export FAKE_INDEX_JSON="$work/index.json"

amd64=sha256:1111111111111111111111111111111111111111111111111111111111111111
arm64=sha256:2222222222222222222222222222222222222222222222222222222222222222

write_index() {
  # write_index PLATFORM... writes an index with one manifest per platform
  # plus one attestation manifest each, unless ATTESTATIONS overrides the
  # attestation count.
  local entries=() p os arch
  for p in "$@"; do
    os=${p%%/*}; arch=${p##*/}
    entries+=("{\"platform\":{\"os\":\"$os\",\"architecture\":\"$arch\"}}")
  done
  local n=${ATTESTATIONS:-$#}
  for ((i = 0; i < n; i++)); do
    entries+=('{"platform":{"os":"unknown","architecture":"unknown"}}')
  done
  printf '{"manifests":[%s]}' "$(IFS=,; echo "${entries[*]}")" >"$FAKE_INDEX_JSON"
}

failures=0
check() {
  local name=$1; shift
  if "$@"; then echo "ok   $name"; else echo "FAIL $name"; failures=$((failures + 1)); fi
}

case_two_platforms() {
  : >"$FAKE_DOCKER_LOG"
  write_index linux/amd64 linux/arm64
  "$script" --version 0.2.0 --platforms linux/amd64,linux/arm64 \
    --repo docker.io/pipestreamai/grpc-qparse \
    "$amd64" "${arm64#sha256:}" >/dev/null
  grep -qxF "buildx imagetools create -t docker.io/pipestreamai/grpc-qparse:latest -t docker.io/pipestreamai/grpc-qparse:0.2.0 docker.io/pipestreamai/grpc-qparse@$amd64 docker.io/pipestreamai/grpc-qparse@$arm64" "$FAKE_DOCKER_LOG" &&
  [[ $(grep -c "imagetools inspect --raw" "$FAKE_DOCKER_LOG") -eq 2 ]]
}

case_no_version_only_latest() {
  : >"$FAKE_DOCKER_LOG"
  write_index linux/amd64 linux/arm64
  "$script" --version '' --platforms linux/amd64,linux/arm64 \
    --repo docker.io/pipestreamai/grpc-qparse "$amd64" "$arm64" >/dev/null
  grep -qxF "buildx imagetools create -t docker.io/pipestreamai/grpc-qparse:latest docker.io/pipestreamai/grpc-qparse@$amd64 docker.io/pipestreamai/grpc-qparse@$arm64" "$FAKE_DOCKER_LOG" &&
  ! grep -q "0\.2\.0" "$FAKE_DOCKER_LOG"
}

case_missing_leg_refuses_before_docker() {
  : >"$FAKE_DOCKER_LOG"
  write_index linux/amd64 linux/arm64
  ! "$script" --version 0.2.0 --platforms linux/amd64,linux/arm64 \
    --repo docker.io/pipestreamai/grpc-qparse "$amd64" >/dev/null 2>&1 &&
  [[ ! -s "$FAKE_DOCKER_LOG" ]]
}

case_missing_platform_in_index_fails() {
  : >"$FAKE_DOCKER_LOG"
  write_index linux/amd64
  ! "$script" --version '' --platforms linux/amd64,linux/arm64 \
    --repo docker.io/pipestreamai/grpc-qparse "$amd64" "$arm64" >/dev/null 2>&1
}

case_missing_attestation_fails() {
  : >"$FAKE_DOCKER_LOG"
  ATTESTATIONS=0 write_index linux/amd64 linux/arm64
  ! "$script" --version '' --platforms linux/amd64,linux/arm64 \
    --repo docker.io/pipestreamai/grpc-qparse "$amd64" "$arm64" >/dev/null 2>&1
}

case_bad_digest_refused() {
  : >"$FAKE_DOCKER_LOG"
  ! "$script" --version '' --platforms linux/amd64 \
    --repo docker.io/pipestreamai/grpc-qparse "sha256:nope" 2>/dev/null &&
  [[ ! -s "$FAKE_DOCKER_LOG" ]]
}

check "two platforms, version and latest tags" case_two_platforms
check "no version publishes only latest" case_no_version_only_latest
check "a missing leg refuses before touching docker" case_missing_leg_refuses_before_docker
check "an index short of a platform fails the gate" case_missing_platform_in_index_fails
check "an index without attestations fails the gate" case_missing_attestation_fails
check "a malformed digest is refused" case_bad_digest_refused

[[ $failures -eq 0 ]] || { echo "$failures case(s) failed" >&2; exit 1; }
echo "test-publish-manifests: OK"
