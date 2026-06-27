FROM --platform=linux/amd64 ubuntu:22.04

RUN apt update && apt install -y \
    build-essential \
    gcc-multilib \
    nasm \
    grub-pc-bin \
    grub-common \
    xorriso \
    mtools \
    exfatprogs \
    qemu-system-x86

WORKDIR /src