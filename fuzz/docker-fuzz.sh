#!/bin/sh
# fuzz/docker-fuzz.sh - run one harness inside a throwaway container.
# the harnesses here are pure, so docker is optional today; use this
# runner for soaks or for any future harness that touches the
# filesystem. the container has no network, a read-only root
# filesystem, and no capabilities.
#
# usage:
#   fuzz/docker-fuzz.sh [harness] [extra libFuzzer args...]
#     harness: fuzz_buf (default) | fuzz_cli
#     e.g. fuzz/docker-fuzz.sh fuzz_cli -runs=100000 -max_len=2048
# seeds: fuzz/seeds/<harness> (mounted read-only)
# env:   RUNS MAXLEN TIMEOUT RSS CORPUSDIR
#   CORPUSDIR (optional) host dir to grow the corpus into; artifacts
#   (crash-*/oom-*/timeout-*) land there too instead of dying with
#   the container.
set -eu
cd "$(dirname "$0")/.."

HARNESS=${1:-fuzz_buf}
case "$HARNESS" in
    fuzz_buf|fuzz_cli) shift ;;
    *) shift 0 ;;
esac

docker build -f fuzz/Dockerfile -t flower-fuzz . >&2

# note: no --memory/--pids-limit - some hosts run cgroup v2 in threaded
# mode and reject them; --network none + --read-only + cap-drop is the
# isolation that matters for a fuzzer process.
CORPUS_ARGS=""
ARTIFACT_PREFIX=/tmp/
[ -n "${CORPUSDIR:-}" ] && {
    CORPUSDIR=$(cd "$CORPUSDIR" && pwd)   # docker -v needs an absolute host path
    CORPUS_ARGS="-v ${CORPUSDIR}:/corpus"
    ARTIFACT_PREFIX=/corpus/               # keep crash-*/oom-* artifacts on host
}

docker run --rm -i \
    --cap-drop ALL \
    --security-opt no-new-privileges \
    --read-only \
    --network none \
    --tmpfs /tmp:rw,size=512m,exec \
    -v "$(pwd)/fuzz/seeds/$HARNESS:/seeds:ro" \
    $CORPUS_ARGS \
    flower-fuzz \
    /bin/sh -c 'mkdir -p /tmp/corpus && exec "$0" "$@"' \
    "/src/fuzz/$HARNESS" \
    -runs="${RUNS:-100000}" -max_len="${MAXLEN:-4096}" \
    -timeout="${TIMEOUT:-15}" -rss_limit_mb="${RSS:-2560}" \
    -artifact_prefix="${ARTIFACT_PREFIX}" \
    ${CORPUSDIR:+/corpus} /tmp/corpus /seeds "$@"
