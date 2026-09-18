FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
        ca-certificates \
        cmake \
        curl \
        g++ \
        git \
        gnupg \
        lsb-release \
        ninja-build \
        python3 \
        python3-pip \
        software-properties-common \
        wget \
    && wget --quiet https://apt.llvm.org/llvm.sh --output-document=/tmp/llvm.sh \
    && chmod +x /tmp/llvm.sh \
    && /tmp/llvm.sh 22 \
    && apt-get install --yes --no-install-recommends clang-format-22 clang-tidy-22 \
    && ln --symbolic /usr/bin/clang++-22 /usr/local/bin/clang++ \
    && ln --symbolic /usr/bin/clang-format-22 /usr/local/bin/clang-format \
    && ln --symbolic /usr/bin/clang-tidy-22 /usr/local/bin/clang-tidy \
    && rm --recursive --force /var/lib/apt/lists/* /tmp/llvm.sh

RUN apt-get update \
    && apt-get install --yes --no-install-recommends make \
    && rm --recursive --force /var/lib/apt/lists/*

COPY tools/quality-requirements.txt /tmp/quality-requirements.txt

RUN python3 -m pip install --break-system-packages --no-cache-dir --requirement /tmp/quality-requirements.txt

WORKDIR /workspace
