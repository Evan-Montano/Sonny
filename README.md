# Project Sonny

TODOing the project description since we are still in progress, but putting the build instructions here because I'm certainly going to forget something.

Yes, I had AI help me format the README because it can make the Markdown syntax prettier than I can.

## Linux Build Instructions

I'm providing examples for **Fedora**, my favorite, and **Ubuntu / WSL Ubuntu**.

## 1. Install Dependencies

The browser-assisted Dukascopy helper uses Node.js and Playwright. Install Node.js and npm, then install the dependencies declared in `package.json` and Playwright's Chromium browser:

```bash
npm install
npx playwright install chromium
```

Run these commands from the Sonny project root. `npm install` uses the existing `package-lock.json` to install the project's pinned dependency tree.

### Fedora

```bash
sudo dnf install \
    clang \
    clang-tools-extra \
    cmake \
    ninja-build \
    nodejs \
    npm \
    git \
    curl \
    libcurl-devel \
    json-devel \
    libsecret-devel \
    libsecret-tools \
    openssl-devel \
    pkgconf-pkg-config \
    libomp-devel \
    openblas-devel \
    libsecret \
    gdb
```

### Ubuntu / WSL Ubuntu

```bash
sudo apt update

sudo apt install \
    build-essential \
    clang \
    clangd \
    cmake \
    ninja-build \
    nodejs \
    npm \
    git \
    curl \
    libcurl4-openssl-dev \
    nlohmann-json3-dev \
    libomp-dev \
    libopenblas-dev \
    libsecret-tools \
    libsecret-1-dev \
    gdb
```

Verify the main tools:

```bash
clang++ --version
cmake --version
ninja --version
git --version
```

CMake 3.22 or newer is required for the Protobuf build instructions below.

Sonny uses **C++26**, so the installed Clang version must support C++26.

**WSL:** Keep the project inside the Linux filesystem (for example `~/Sonny`) rather than `/mnt/c/...`.

---

## 2. Clone Sonny

```bash
cd ~
git clone https://github.com/Evan-Montano/Sonny.git Sonny
cd ~/Sonny
```

The project should be located at:

```text
~/Sonny/
```

---

## 3. Clone FAISS

FAISS is kept inside the Sonny directory as a normal Git repository. It is **not** a Git submodule.

```bash
cd ~/Sonny

git clone https://github.com/facebookresearch/faiss.git faiss
```

The resulting layout is:

```text
~/Sonny/
├── CMakeLists.txt
├── src/
├── faiss/
└── build/
```

---

## 4. Build and Install FAISS

FAISS must be built before Sonny.

```bash
cd ~/Sonny/faiss

cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$HOME/.local/faiss" \
    -DFAISS_ENABLE_GPU=OFF \
    -DFAISS_ENABLE_PYTHON=OFF \
    -DBUILD_TESTING=OFF

cmake --build build --target faiss
cmake --install build
```

FAISS will be installed to:

```text
~/.local/faiss
```

---

## 5. Clone, Build, and Install Google Protocol Buffers

Sonny uses Google's Protocol Buffers (Protobuf) library to deserialize binary messages received through the Webull MQTT quote stream.

Protobuf consists of two important components:

- **libprotobuf:** The C++ runtime library used to deserialize messages.
- **protoc:** The Protocol Buffers compiler, which generates C++ source and header files from `.proto` definitions.

Both must be installed.

To maintain consistent versions between Fedora and Ubuntu, Sonny builds Protobuf from the same pinned source release in both environments.

Unlike FAISS, Protobuf's source, build artifacts, and installation are all kept together in the Sonny directory.

### Clone Protobuf

```bash
cd ~/Sonny

git clone --depth 1 --branch v36.2 \
    https://github.com/protocolbuffers/protobuf.git protobuf
```

### Configure Protobuf

```bash
cmake -S ~/Sonny/protobuf \
    -B ~/Sonny/protobuf/build \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$HOME/Sonny/protobuf/install" \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_CXX_STANDARD=17 \
    -Dprotobuf_BUILD_TESTS=OFF \
    -Dprotobuf_BUILD_SHARED_LIBS=OFF
```

A few notes:

- Protobuf requires C++17 or newer. It can be built with C++17 independently of Sonny's C++26 standard.
- Tests are disabled to avoid building Protobuf's test suite.
- Static libraries are used for consistency with Sonny's other statically built dependencies.
- CMake automatically fetches Abseil if it is not already installed.

### Build and Install

```bash
cmake --build ~/Sonny/protobuf/build --parallel 4

cmake --install ~/Sonny/protobuf/build
```

Protobuf will be installed to:

```text
~/Sonny/protobuf/install
```

### Verify Installation

Check the Protobuf compiler:

```bash
~/Sonny/protobuf/install/bin/protoc --version
```

Expected output:

```text
libprotoc 36.2
```

Verify that the headers and libraries were installed:

```bash
test -f ~/Sonny/protobuf/install/include/google/protobuf/message.h \
    && echo "Protobuf headers installed"

find ~/Sonny/protobuf/install -name 'libprotobuf*'
```

---

## 6. Build Sonny

Return to the Sonny root directory:

```bash
cd ~/Sonny
```

Configure Sonny:

```bash
cmake -S . -B build -G Ninja \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_PREFIX_PATH="$HOME/.local/faiss;$HOME/Sonny/protobuf/install"
```

Build:

```bash
cmake --build build
```

FTXUI is downloaded automatically by CMake through `FetchContent`.

---

## 7. Run Sonny

```bash
./build/sonny
```

---

## 8. Normal Development

After the initial setup, FAISS and Protobuf do not need to be rebuilt every time Sonny changes.

```bash
cd ~/Sonny
cmake --build build
./build/sonny
```

---

## 9. Clean Rebuild

If CMake or the build configuration becomes corrupted, remove the build directories:

```bash
cd ~/Sonny

rm -rf build
rm -rf faiss/build
```

For a complete Protobuf rebuild, also remove its build and installation directories:

```bash
rm -rf protobuf/build
rm -rf protobuf/install
```

Then repeat **Step 4**, **Step 5**, and **Step 6**.

---

## Directory Reference

| Directory | Purpose |
|---|---|
| `~/Sonny` | Sonny source |
| `~/Sonny/faiss` | FAISS source |
| `~/Sonny/build` | Sonny build |
| `~/Sonny/faiss/build` | FAISS build |
| `~/Sonny/protobuf` | Protobuf source |
| `~/Sonny/protobuf/build` | Protobuf build |
| `~/Sonny/protobuf/install` | Installed Protobuf |
| `~/.local/faiss` | Installed FAISS |

## CMake Configuration

Sonny currently uses:

- **C++26**
- **Clang / Clang++**
- **Ninja**
- **FTXUI 7.0.3**
- **FAISS**
- **Google Protocol Buffers 36.2**
- **libcurl**
- **nlohmann/json**
- **OpenMP**
- **Paho MQTT C++ / C** (fetched and built statically with TLS by CMake)
- **libsecret** (read-only Secret Service access)

See [docs/webull-streaming.md](docs/webull-streaming.md) for OpenAPI application setup, Secret Service provisioning, and live/sandbox quote-stream details.

Linux builds are configured as **Debug** builds with debugging symbols and optimizations disabled, for now while development is still in progress.