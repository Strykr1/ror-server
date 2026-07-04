#!/usr/bin/env bash
set -euo pipefail

readonly SOURCE_DIR=/workspace
readonly BUILD_DIR="${SOURCE_DIR}/build/container-baseline"
readonly METADATA_DIR="${BUILD_DIR}/build-metadata"
readonly BINARY="${BUILD_DIR}/bin/rorserver"
readonly NOTES_FILE="${SOURCE_DIR}/CONTAINER-BUILD-NOTES.md"

readonly BUILDER_IMAGE="ror-server-builder:ubuntu-24.04"
readonly BUILDER_BASE_IMAGE="ubuntu:24.04@sha256:52df9b1ee71626e0088f7d400d5c6b5f7bb916f8f0c82b474289a4ece6cf3faf"
readonly RUNTIME_DIGEST="158c98fe9395f81f13de72b3fae3ce1b8542a8c7389d2e0c5e298d1d281d72e2"
readonly RUNTIME_IMAGE="ghcr.io/parkervcp/yolks:ubuntu@sha256:${RUNTIME_DIGEST}"
readonly ROR_CONAN_REMOTE="https://git.anotherfoxguy.com/api/packages/rorbot/conan"
readonly PARALLEL_JOBS="$(nproc)"

test -n "${CONAN_HOME:-}"
test "$(id -un)" = builder
test "$(uname -m)" = "x86_64"
test -w "${SOURCE_DIR}"
test -w "${CONAN_HOME}"
test -f "${SOURCE_DIR}/CMakeLists.txt"
test -f "${SOURCE_DIR}/conanfile.py"
test -f "${SOURCE_DIR}/cmake/conan_provider.cmake"

conan profile detect --force
conan remote add ror-conan "${ROR_CONAN_REMOTE}" --force

mkdir -p "${BUILD_DIR}"

cmake \
    -S "${SOURCE_DIR}" \
    -B "${BUILD_DIR}" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DRORSERVER_WITH_ANGELSCRIPT=ON \
    -DRORSERVER_WITH_CURL=ON \
    -DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=cmake/conan_provider.cmake

cmake \
    --build "${BUILD_DIR}" \
    --target rorserver \
    --parallel "${PARALLEL_JOBS}"

test -x "${BINARY}"
mkdir -p "${METADATA_DIR}"

file "${BINARY}" \
    >"${METADATA_DIR}/file.txt"

ldd "${BINARY}" \
    >"${METADATA_DIR}/ldd.txt"

sha256sum "${BINARY}" \
    >"${METADATA_DIR}/sha256sum.txt"

readelf -d "${BINARY}" \
    >"${METADATA_DIR}/readelf-d.txt"

strings --all "${BINARY}" \
    >"${METADATA_DIR}/strings.txt"

: >"${METADATA_DIR}/strings-checks.txt"

for expected_string in \
    RoRnet_2.45 \
    ScriptEngine \
    AngelScript \
    printstats \
    CURL
do
    if grep \
        --fixed-strings \
        --line-number \
        "${expected_string}" \
        "${METADATA_DIR}/strings.txt" \
        >"${METADATA_DIR}/strings-${expected_string}.txt"
    then
        printf 'FOUND: %s\n' "${expected_string}"
    else
        printf 'NOT FOUND: %s\n' "${expected_string}"
    fi
done | tee "${METADATA_DIR}/strings-checks.txt"

{
    printf '# Container Baseline Build Notes\n\n'

    printf '## Images\n\n'
    printf -- '- Builder image: `%s`\n' "${BUILDER_IMAGE}"
    printf -- '- Builder base: `%s`\n' "${BUILDER_BASE_IMAGE}"
    printf -- '- Pterodactyl runtime: `%s`\n\n' "${RUNTIME_IMAGE}"

    printf '## Source\n\n'
    printf -- '- Git commit: `%s`\n' \
        "$(git -C "${SOURCE_DIR}" rev-parse HEAD)"
    printf -- '- Source: `%s`\n' "${SOURCE_DIR}"
    printf -- '- Build directory: `%s`\n' "${BUILD_DIR}"
    printf -- '- Target: `rorserver`\n'
    printf -- '- Parallel jobs: `%s`\n\n' "${PARALLEL_JOBS}"

    printf '## Environment\n\n'
    printf '```text\n'
    cat /etc/os-release
    getconf GNU_LIBC_VERSION
    gcc --version | head -n 1
    cmake --version | head -n 1
    ninja --version
    conan --version
    python3 --version
    uname -m
    printf '```\n\n'

    printf '## Build configuration\n\n'
    printf '```text\n'
    printf 'Generator=Ninja\n'
    printf 'CMAKE_BUILD_TYPE=RelWithDebInfo\n'
    printf 'RORSERVER_WITH_ANGELSCRIPT=ON\n'
    printf 'RORSERVER_WITH_CURL=ON\n'
    printf 'CMAKE_PROJECT_TOP_LEVEL_INCLUDES=cmake/conan_provider.cmake\n'
    printf 'Conan remote=%s\n' "${ROR_CONAN_REMOTE}"
    printf 'Conan cache=%s\n' "${CONAN_HOME}"
    printf '```\n\n'

    printf '## Commands\n\n'
    printf '```bash\n'
    printf 'conan profile detect --force\n'
    printf 'conan remote add ror-conan %s --force\n' \
        "${ROR_CONAN_REMOTE}"
    printf 'cmake -S /workspace -B /workspace/build/container-baseline -G Ninja \\\n'
    printf '  -DCMAKE_BUILD_TYPE=RelWithDebInfo \\\n'
    printf '  -DRORSERVER_WITH_ANGELSCRIPT=ON \\\n'
    printf '  -DRORSERVER_WITH_CURL=ON \\\n'
    printf '  -DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=cmake/conan_provider.cmake\n'
    printf 'cmake --build /workspace/build/container-baseline \\\n'
    printf '  --target rorserver --parallel %s\n' "${PARALLEL_JOBS}"
    printf 'file %s\n' "${BINARY}"
    printf 'ldd %s\n' "${BINARY}"
    printf 'sha256sum %s\n' "${BINARY}"
    printf 'readelf -d %s\n' "${BINARY}"
    printf 'strings --all %s\n' "${BINARY}"
    printf '```\n\n'

    printf '## Metadata\n\n'
    printf 'Results: `%s`\n' "${METADATA_DIR}"
} >"${NOTES_FILE}"
