FROM ubuntu:24.04@sha256:52df9b1ee71626e0088f7d400d5c6b5f7bb916f8f0c82b474289a4ece6cf3faf

ARG HOST_UID
ARG HOST_GID

ENV DEBIAN_FRONTEND=noninteractive
ENV VIRTUAL_ENV=/opt/ror-build-venv
ENV PATH="${VIRTUAL_ENV}/bin:${PATH}"
ENV CONAN_HOME=/conan-cache

RUN test -n "${HOST_UID}" \
    && test -n "${HOST_GID}" \
    && test "${HOST_UID}" -gt 0 \
    && test "${HOST_GID}" -gt 0

RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        git \
        curl \
        python3 \
        python3-pip \
        python3-venv \
        pkg-config \
        ca-certificates \
        gdb \
        strace \
        lsof \
        file \
        binutils \
    && rm -rf /var/lib/apt/lists/*

RUN python3 -m venv "${VIRTUAL_ENV}" \
    && "${VIRTUAL_ENV}/bin/pip" install \
        --no-cache-dir \
        "conan>=2.0.5,<3"

RUN set -eux; \
    existing_uid_user="$(getent passwd "${HOST_UID}" | cut -d: -f1 || true)"; \
    if [ -n "${existing_uid_user}" ]; then \
        userdel --remove "${existing_uid_user}"; \
    fi; \
    if getent passwd builder >/dev/null; then \
        userdel --remove builder; \
    fi; \
    if getent group builder >/dev/null; then \
        existing_builder_gid="$(getent group builder | cut -d: -f3)"; \
        if [ "${existing_builder_gid}" != "${HOST_GID}" ]; then \
            groupmod --new-name "builder-existing-${existing_builder_gid}" builder; \
        fi; \
    fi; \
    existing_gid_group="$(getent group "${HOST_GID}" | cut -d: -f1 || true)"; \
    if [ -n "${existing_gid_group}" ]; then \
        if [ "${existing_gid_group}" != builder ]; then \
            groupmod --new-name builder "${existing_gid_group}"; \
        fi; \
    else \
        groupadd --gid "${HOST_GID}" builder; \
    fi; \
    useradd \
        --uid "${HOST_UID}" \
        --gid builder \
        --home-dir /home/builder \
        --create-home \
        --shell /bin/bash \
        builder; \
    mkdir -p /conan-cache; \
    chown -R builder:builder /conan-cache

WORKDIR /workspace

USER builder

CMD ["/bin/bash"]
