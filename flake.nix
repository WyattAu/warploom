{
  description = "Warploom C++ Development Environment for CachyOS with Qt6, Vulkan, GCC 13, and Clang 19";

  inputs = {
    # Use nixos-unstable for latest packages (pinned via flake.lock for TM-LX-001 security)
    nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      # System architecture targeting CachyOS (Arch Linux derivative)
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
    in
    {
      # Development shells - provide reproducible environments (ADR-027)
      devShells.${system} = {
        # Default development shell with all toolchains (REQ-002-006)
        default = pkgs.mkShell {
          # Core compiler toolchains
          buildInputs = with pkgs; [
            # GCC 13 toolchain for C++23 development (REQ-002-002)
            gcc13

            # Clang 19 toolchain for C++23 development (REQ-002-003)
            llvmPackages_19.clang
            llvmPackages_19.llvm
            llvmPackages_19.bintools

            # Build system tools (REQ-002-006)
            cmake
            ninja
            ccache

            # Package managers (REQ-002-006)
            # Note: Conan removed due to nixpkgs dependency conflict
            # Using CPM (CMake Package Manager) instead
            vcpkg

            # Python development environment (REQ-002-006)
            python3
            python3Packages.pip
            python3Packages.pytest
            python3Packages.pytest-cov
            gcovr

            # Qt6 dependencies (REQ-002-004)
            qt6.qtbase
            qt6.qtdeclarative
            qt6.qttools
            qt6.qtwayland
            qt6.qtsvg
            qt6.qtimageformats

            # Vulkan dependencies (REQ-002-005)
            vulkan-headers
            vulkan-loader
            vulkan-tools
            vulkan-validation-layers
            vulkan-extension-layer

            # Vulkan shader tools
            glslang
            shaderc
            spirv-tools
            spirv-cross

            # Development tools (REQ-002-006)
            clang-tools
            cppcheck
            clang-analyzer
            doxygen
            graphviz

            # Debugging tools (REQ-002-006)
            gdb
            lldb
            valgrind

            # Performance tools (REQ-002-006)
            perf

            # X11/Wayland development libraries (updated to non-deprecated names)
            libX11
            libXrandr
            libXinerama
            libXcursor
            xcbproto
            libxcb
            libxkbcommon
            wayland
            wayland-protocols

            # Native build inputs for build configuration
            pkg-config
            git
            curl
            jq
          ];

          # Environment setup for development shell (REQ-002-006)
          shellHook = ''
            echo ">> Loaded Warploom C++ Development Environment"
            echo ">> GCC 13 and Clang 19 toolchains available"
            echo ">> Qt6 and Vulkan graphics libraries loaded"
             echo ">> vcpkg package manager ready"
            echo ">> Python environment ready"
            echo ""
            
            # Qt6 platform configuration (REQ-002-004)
            export QT_QPA_PLATFORM=xcb
            export QT_PLUGIN_PATH=${pkgs.qt6.qtbase}/lib/qt-6/plugins
            export QMAKE=${pkgs.qt6.qtbase}/bin/qmake
            
            # Vulkan configuration (REQ-002-005)
            # NOTE: Do NOT set LD_LIBRARY_PATH globally as it breaks linking
            # Instead, use a wrapper script to run applications with proper library paths
            # See run-pong.sh for example
            
            # Vulkan layers from Nix
            export VK_LAYER_PATH=${pkgs.vulkan-validation-layers}/share/vulkan/explicit_layer.d
            
            # CMake configuration (REQ-002-007)
            export CMAKE_GENERATOR="Ninja"
            export CMAKE_BUILD_PARALLEL_LEVEL=$(nproc)
            export CMAKE_PREFIX_PATH="${pkgs.qt6.qtbase}:${pkgs.vulkan-headers}:${pkgs.vulkan-loader}:$CMAKE_PREFIX_PATH"
            export CMAKE_EXPORT_COMPILE_COMMANDS=ON
            
            # Fix _FORTIFY_SOURCE conflict with -O0 optimization
            export CMAKE_CXX_FLAGS="-U_FORTIFY_SOURCE $CMAKE_CXX_FLAGS"
            export CMAKE_C_FLAGS="-U_FORTIFY_SOURCE $CMAKE_C_FLAGS"
            
            # Compiler cache configuration
            export CCACHE_DIR=$PWD/.ccache
            
            # Default to Clang for better diagnostics
            export CC=clang
            export CXX=clang++
            export CMAKE_C_COMPILER=clang
            export CMAKE_CXX_COMPILER=clang++
          '';

          env = {
            QT_QPA_PLATFORM = "xcb";
            CMAKE_GENERATOR = "Ninja";
          };
        };
      };
    };
}
