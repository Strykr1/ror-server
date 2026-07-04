# Container Baseline Build Notes

## Images

- Builder image: `ror-server-builder:ubuntu-24.04`
- Builder base: `ubuntu:24.04@sha256:52df9b1ee71626e0088f7d400d5c6b5f7bb916f8f0c82b474289a4ece6cf3faf`
- Pterodactyl runtime: `ghcr.io/parkervcp/yolks:ubuntu@sha256:158c98fe9395f81f13de72b3fae3ce1b8542a8c7389d2e0c5e298d1d281d72e2`

## Source

- Git commit: `8ed60cc9ee7dfbce6ea9c6d8290eaeb5a5a46d5a`
- Source: `/workspace`
- Build directory: `/workspace/build/container-baseline`
- Target: `rorserver`
- Parallel jobs: `24`

## Environment

```text
PRETTY_NAME="Ubuntu 24.04.4 LTS"
NAME="Ubuntu"
VERSION_ID="24.04"
VERSION="24.04.4 LTS (Noble Numbat)"
VERSION_CODENAME=noble
ID=ubuntu
ID_LIKE=debian
HOME_URL="https://www.ubuntu.com/"
SUPPORT_URL="https://help.ubuntu.com/"
BUG_REPORT_URL="https://bugs.launchpad.net/ubuntu/"
PRIVACY_POLICY_URL="https://www.ubuntu.com/legal/terms-and-policies/privacy-policy"
UBUNTU_CODENAME=noble
LOGO=ubuntu-logo
glibc 2.39
gcc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0
cmake version 3.28.3
1.11.1
Conan version 2.30.0
Python 3.12.3
x86_64
```

## Build configuration

```text
Generator=Ninja
CMAKE_BUILD_TYPE=RelWithDebInfo
RORSERVER_WITH_ANGELSCRIPT=ON
RORSERVER_WITH_CURL=ON
CMAKE_PROJECT_TOP_LEVEL_INCLUDES=cmake/conan_provider.cmake
Conan remote=https://git.anotherfoxguy.com/api/packages/rorbot/conan
Conan cache=/conan-cache
```

## Commands

```bash
conan profile detect --force
conan remote add ror-conan https://git.anotherfoxguy.com/api/packages/rorbot/conan --force
cmake -S /workspace -B /workspace/build/container-baseline -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DRORSERVER_WITH_ANGELSCRIPT=ON \
  -DRORSERVER_WITH_CURL=ON \
  -DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=cmake/conan_provider.cmake
cmake --build /workspace/build/container-baseline \
  --target rorserver --parallel 24
file /workspace/build/container-baseline/bin/rorserver
ldd /workspace/build/container-baseline/bin/rorserver
sha256sum /workspace/build/container-baseline/bin/rorserver
readelf -d /workspace/build/container-baseline/bin/rorserver
strings --all /workspace/build/container-baseline/bin/rorserver
```

## Metadata

Results: `/workspace/build/container-baseline/build-metadata`
