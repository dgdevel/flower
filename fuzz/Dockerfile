# fuzz/Dockerfile - sandbox for the fuzz harnesses. the harnesses here
# are pure (buffer and arg parser), so docker is optional today;
# this exists as the ready-made sandbox for any future harness that
# touches the filesystem.
#
# build context is the repo root (see fuzz/docker-fuzz.sh):
#   docker build -f fuzz/Dockerfile -t flower-fuzz .
#
# the harnesses are built here, at image time, on a writable rootfs;
# at runtime docker mounts the rootfs read-only and /tmp becomes a
# throwaway tmpfs. the seeds are mounted from the host at runtime, so
# a rebuilt image never masks seed edits.
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        clang libclang-rt-14-dev make git \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# build once here; the artifacts are owned by root in /src/fuzz, and
# the runtime user only reads them
RUN make -C /src fuzz

# non-root, no matter what the host user id is
RUN useradd -m fuzzer
USER fuzzer
ENV HOME=/tmp

# run as: docker run ... flower-fuzz /src/fuzz/fuzz_buf <libFuzzer args> /seeds
CMD ["/bin/sh", "-c"]
