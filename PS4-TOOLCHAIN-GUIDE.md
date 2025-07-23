# OpenOrbis PS4 LLVM Toolchain Guide

This LLVM 20 toolchain has been enhanced with PS4 (PlayStation 4) support for homebrew development using the OpenOrbis SDK.

## Overview

This toolchain includes PS4-specific modifications to:
- **Clang**: PS4 target support with init arrays enabled by default
- **LLD**: PS4 ELF linker with SCE-specific relocations and formats
- **libcxx/libcxxabi**: PS4 threading and system library support

## Building the Toolchain

### Prerequisites
```bash
# Ubuntu/Debian
sudo apt-get install build-essential cmake ninja-build python3

# macOS
brew install cmake ninja

# Fedora
sudo dnf install cmake ninja-build gcc-c++
```

### Build Instructions
```bash
# Configure
mkdir build && cd build
cmake -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_ENABLE_PROJECTS="clang;lld" \
  -DLLVM_ENABLE_RUNTIMES="libcxx;libcxxabi;libunwind" \
  -DLLVM_TARGETS_TO_BUILD="X86" \
  -DLLVM_DEFAULT_TARGET_TRIPLE="x86_64-scei-ps4" \
  ../llvm

# Build
ninja -j$(nproc) clang lld

# Optional: Install to system
sudo ninja install
```

## PS4 Development Usage

### Basic Compilation

```bash
# Compile C++ for PS4
clang++ -target x86_64-scei-ps4 \
  -fuse-init-array \
  -ffreestanding \
  -nostdlib \
  -fno-builtin \
  -fPIC \
  -c main.cpp -o main.o

# Compile C for PS4
clang -target x86_64-scei-ps4 \
  -fuse-init-array \
  -ffreestanding \
  -nostdlib \
  -fno-builtin \
  -fPIC \
  -c code.c -o code.o
```

### Linking PS4 Executables

```bash
# Link PS4 SELF executable (with ASLR)
ld.lld -m elf_x86_64_ps4 \
  -pie \
  --script=sce_sys/link.x \
  --eh-frame-hdr \
  -o eboot.bin \
  crt0.o main.o -lkernel -lc -lc++

# Link PS4 library (PRX)
ld.lld -m elf_x86_64_ps4 \
  -shared \
  --script=sce_sys/prx.x \
  -o mylib.prx \
  module.o -lkernel
```

### PS4-Specific Compiler Flags

| Flag | Description |
|------|-------------|
| `-target x86_64-scei-ps4` | Target PS4 platform |
| `-fuse-init-array` | Use init arrays (default for PS4) |
| `-ffreestanding` | Freestanding environment |
| `-fPIC` | Position-independent code (required) |
| `-fvisibility=hidden` | Hide symbols by default |
| `-fno-exceptions` | Disable C++ exceptions |
| `-fno-rtti` | Disable C++ RTTI |

### PS4-Specific Linker Flags

| Flag | Description |
|------|-------------|
| `-m elf_x86_64_ps4` | PS4 ELF emulation |
| `-pie` | Position-independent executable |
| `--eh-frame-hdr` | Create .eh_frame_hdr section |
| `--script=link.x` | Use PS4 linker script |
| `-z max-page-size=0x4000` | 16KB page size |
| `-z common-page-size=0x4000` | 16KB common page size |

## Integration with OpenOrbis SDK

### Environment Setup
```bash
# Set OpenOrbis SDK path
export OO_PS4_TOOLCHAIN=/path/to/this/llvm/build
export OPENORBIS=/path/to/OpenOrbis-PS4-Toolchain

# Add to PATH
export PATH=$OO_PS4_TOOLCHAIN/bin:$PATH
```

### Using with OpenOrbis Makefiles
```makefile
# In your project Makefile
CC = clang
CXX = clang++
LD = ld.lld
CFLAGS += -target x86_64-scei-ps4
CXXFLAGS += -target x86_64-scei-ps4
LDFLAGS += -m elf_x86_64_ps4
```

## PS4 ELF Format Details

### Executable Types
- `ET_SCE_EXEC` (0xfe00): Static PS4 executable
- `ET_SCE_EXEC_ASLR` (0xfe10): PS4 executable with ASLR
- `ET_SCE_DYNAMIC` (0xfe18): PS4 shared object (PRX)

### Program Headers
- `PT_SCE_DYNLIBDATA` (0x61000000): Dynamic library data
- `PT_SCE_PROC_PARAM` (0x61000001): Process parameters
- `PT_SCE_RELRO` (0x61000010): Read-only after relocation

### Dynamic Tags
The toolchain supports PS4-specific dynamic tags (DT_SCE_*) for:
- Module information
- Import/export libraries
- Symbol tables
- Relocations
- NID (Name ID) system

## Testing Your Build

### Verify PS4 Target
```bash
# Check default target
clang --version | grep Target

# Test PS4 compilation
echo 'int main() { return 0; }' > test.c
clang -target x86_64-scei-ps4 -c test.c -o test.o
file test.o  # Should show ELF 64-bit LSB relocatable
```

### Check Linker Support
```bash
# Verify PS4 emulation
ld.lld --help | grep ps4

# Test linking
ld.lld -m elf_x86_64_ps4 --help
```

## Troubleshooting

### Common Issues

1. **"Unknown target"**: Ensure LLVM was built with X86 target support
2. **Linking errors**: Verify you have PS4 libraries and startup files
3. **Missing symbols**: Link against appropriate PS4 system libraries

### Debug Build
For development and debugging of the toolchain itself:
```bash
cmake -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DLLVM_ENABLE_ASSERTIONS=ON \
  -DLLVM_ENABLE_PROJECTS="clang;lld" \
  -DLLVM_TARGETS_TO_BUILD="X86" \
  ../llvm
```

## PS4-Specific Modifications

This toolchain includes the following PS4-specific changes:

1. **libcxx**: PS4 threading support, filesystem exceptions
2. **Clang Driver**: Init arrays enabled by default for PS4
3. **ELF Constants**: ELFOSABI_PS4, ET_SCE_*, PT_SCE_*
4. **Dynamic Tags**: Full DT_SCE_* support
5. **LLD**: PS4 emulation, OSABI handling, custom sections
6. **Symbol Generation**: NID support for PS4 symbols

## References

- [OpenOrbis PS4 Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain)
- [PS4 Developer Wiki](https://www.psdevwiki.com/ps4/)
- [LLVM Documentation](https://llvm.org/docs/)

## License

This PS4-enhanced LLVM toolchain maintains the original LLVM license terms.
The PS4-specific modifications are part of the OpenOrbis project.