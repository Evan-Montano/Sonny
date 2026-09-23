# Project Sonny

TODOing the project description since we are still in progress, but putting the build instructions here because I'm certainly going to forget something.

Yes, I had AI help me format the readme because it can make the markdown syntax prettier than I can.

## Linux Build Instructions

I'm providing examples for **Fedora**, my favorite, and **Ubuntu / WSL Ubuntu**.

## 1. Install Dependencies

### Fedora

```bash
sudo dnf install \
    clang \
    clang-tools-extra \
    cmake \
    ninja-build \
    git \
    curl \
    libcurl-devel \
    json-devel \
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
    git \
    curl \
    libcurl4-openssl-dev \
    nlohmann-json3-dev \
    libomp-dev \
    libopenblas-dev \
    libsecret-tools \
    gdb
```

Verify the main tools:

```bash
clang++ --version
cmake --version
ninja --version
git --version
```

Sonny uses **C++26**, so the installed Clang version must support C++26.

> **WSL:** Keep the project inside the Linux filesystem (for example `~/Sonny`) rather than `/mnt/c/...`.

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

cmake -S . -B build
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

## 5. Build Sonny

Return to the Sonny root directory:

```bash
cd ~/Sonny
```

Configure Sonny:

```bash
cmake -S . -B build -G Ninja \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_PREFIX_PATH="$HOME/.local/faiss"
```

Build:

```bash
cmake --build build
```

FTXUI is downloaded automatically by CMake through `FetchContent`.

---

## 6. Run Sonny

```bash
./build/sonny
```

---

## 7. Normal Development

After the initial setup, FAISS does not need to be rebuilt every time Sonny changes.

```bash
cd ~/Sonny
cmake --build build
./build/sonny
```

---

## 8. Clean Rebuild

If CMake or the build configuration becomes corrupted, remove both build directories:

```bash
cd ~/Sonny

rm -rf build
rm -rf faiss/build
```

Then repeat **Step 4** and **Step 5**.

---

## Directory Reference

| Directory             | Purpose         |
| --------------------- | --------------- |
| `~/Sonny`             | Sonny source    |
| `~/Sonny/faiss`       | FAISS source    |
| `~/Sonny/build`       | Sonny build     |
| `~/Sonny/faiss/build` | FAISS build     |
| `~/.local/faiss`      | Installed FAISS |

## CMake Configuration

Sonny currently uses:

* **C++26**
* **Clang / Clang++**
* **Ninja**
* **FTXUI 7.0.3**
* **FAISS**
* **libcurl**
* **nlohmann/json**
* **OpenMP**

Linux builds are configured as **Debug** builds with debugging symbols and optimizations disabled, for now while development is still in progress.