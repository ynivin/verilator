FROM fedora:latest

RUN dnf update -y

# Verilator dependencies
RUN dnf install -y git autoconf flex bison make cmake g++ \
    zlib-devel libstdc++-static glibc-static perl python3 gdb which

RUN cpan -i Pod::Perldoc Unix::Processors Parallel::Forker Bit::Vector

ARG USER=verilator-dev
ARG USER_UID=1000
ARG USER_GID=$USER_UID

RUN groupadd --gid $USER_GID $USER \
    && useradd -s /bin/bash --uid $USER_UID --gid $USER_GID -m $USER

RUN echo $USER ALL=\(root\) NOPASSWD:ALL > /etc/sudoers.d/$USER \
    && chmod 0440 /etc/sudoers.d/$USER

USER $USER