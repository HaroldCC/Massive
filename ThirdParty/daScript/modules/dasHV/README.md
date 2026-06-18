# dasHV - bindings for HV - cross platform network library

## PREREQUISITES:
- **OpenSSL** (works with `3.5.1`)

## Installation:

### Linux (Debian/Ubuntu):
```bash
sudo apt update
sudo apt install openssl=3.*
```

### macOS (Homebrew):
```bash
brew install openssl@3
```

### Windows:
Using official installer:
- For x86-64 download 3.5.1 installer from https://slproweb.com/download/Win64OpenSSL-3_5_1.exe
- For x86 download 3.5.1 installer from https://slproweb.com/download/Win32OpenSSL-3_5_1.exe

Or via vcpkg:
```powershell
vcpkg install openssl:x64-windows --version=3.5.1 # for x86-64
vcpkg install openssl:x86-windows --version=3.5.1 # for x86
```

If OpenSSL is missing on Windows it will be built from sources.

*Troubleshooting:*
- OpenSSL build requires Strawberry Perl
- Under Ninja you may need to run under VS Developer Command Prompt to ensure all variables are set for OpenSSL build
