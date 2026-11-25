# Lean x86_64 Linux build environment for QNX SDP 8.0 on Apple Silicon.
# The QNX host tools are Linux x86_64 binaries, so this runs under Docker's x86 emulation.
FROM ubuntu:22.04

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      bash ca-certificates make file bc cpio xxd rsync \
      openssh-client openssl uuid-runtime python3 perl \
      locales && \
    locale-gen en_US.UTF-8 && \
    rm -rf /var/lib/apt/lists/*
ENV LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8

# Match the Mac user so files written to the mounted home keep the right owner.
ARG USER_NAME
ARG USER_ID
ARG GROUP_ID
RUN (getent group ${GROUP_ID} || groupadd --gid ${GROUP_ID} hostgroup) && \
    useradd --uid ${USER_ID} --gid ${GROUP_ID} --no-log-init --create-home ${USER_NAME}
USER ${USER_NAME}
WORKDIR /home/${USER_NAME}
