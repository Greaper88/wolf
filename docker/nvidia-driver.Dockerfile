# Driver-only packaging, adapted from GOW's images/nvidia-driver/Dockerfile.
# This never installs or changes the host kernel module.
FROM ubuntu:22.04 AS nvidia-installer
ARG NV_VERSION
RUN apt-get update && apt-get install -y --no-install-recommends \
    curl ca-certificates kmod pkg-config libglvnd-dev python3 \
    && rm -rf /var/lib/apt/lists/*
COPY nvidia-driver-volume.py /opt/wolf/nvidia-driver-volume.py
RUN mkdir -p /usr/nvidia \
    && curl --fail --location --retry 3 \
        "https://download.nvidia.com/XFree86/Linux-x86_64/${NV_VERSION}/NVIDIA-Linux-x86_64-${NV_VERSION}.run" \
        -o /tmp/nvidia.run \
    && sh /tmp/nvidia.run --silent -z --skip-depmod --skip-module-unload \
        --no-nvidia-modprobe --no-kernel-modules --no-kernel-module-source \
        --opengl-prefix=/usr/nvidia --wine-prefix=/usr/nvidia \
        --utility-prefix=/usr/nvidia --utility-libdir=lib \
        --compat32-prefix=/usr/nvidia --compat32-libdir=lib32 \
        --egl-external-platform-config-path=/usr/nvidia/share/egl/egl_external_platform.d \
        --glvnd-egl-config-path=/usr/nvidia/share/glvnd/egl_vendor.d --no-distro-scripts \
    && mkdir -p /usr/nvidia/share/vulkan/icd.d \
    && cp /etc/vulkan/icd.d/nvidia_icd.json /usr/nvidia/share/vulkan/icd.d/ \
    && python3 /opt/wolf/nvidia-driver-volume.py --seal-volume /usr/nvidia "${NV_VERSION}" \
    && rm /tmp/nvidia.run

FROM ubuntu:22.04
ARG NV_VERSION
RUN apt-get update && apt-get install -y --no-install-recommends python3 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=nvidia-installer /usr/nvidia /usr/nvidia
COPY nvidia-driver-volume.py /opt/wolf/nvidia-driver-volume.py
LABEL wolf.nvidia.managed="true" wolf.nvidia.version="${NV_VERSION}" wolf.nvidia.schema="1"
CMD ["python3", "/opt/wolf/nvidia-driver-volume.py", "--help"]
