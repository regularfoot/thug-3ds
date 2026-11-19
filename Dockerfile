FROM devkitpro/devkitarm:20260610@sha256:116afba8df8453961de2936ffab20dd441edf4d682856c1ec8b0e53d7ed0bbf5
RUN dkp-pacman -S --needed --noconfirm 3ds-dev && apt-get update && apt-get install -y --no-install-recommends python3 cmake ninja-build make git ccache && rm -rf /var/lib/apt/lists/*
WORKDIR /project
