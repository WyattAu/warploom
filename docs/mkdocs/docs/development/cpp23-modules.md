# C++23 Modules Guide

This guide explains how to use C++23 modules in the Warploom project.

## Overview

C++23 introduces modules as a modern way to organize and distribute code, replacing traditional header files with module interfaces.

## Creating a Module

1. Create a module interface file with `.cppm` extension:

```cpp
export module my_module;

export int add(int a, int b) {
    return a + b;
}
```

2. Add the module file to your CMakeLists.txt:

```cmake
list(APPEND sources ${CMAKE_CURRENT_LIST_DIR}/src/my_module.cppm)
```

3. Ensure CMake is configured for modules:

```cmake
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_MODULE_STD ON)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)
```

## Importing Modules

In your code, import modules instead of including headers:

```cpp
import my_module;

int main() {
    return add(2, 3);  // 5
}
```

## Benefits

- Faster compilation due to reduced header parsing
- Better encapsulation
- No header guards needed
- Clearer dependencies

## Compiler Support

- GCC 11+
- Clang 14+
- MSVC 2022+

Ensure your build system targets these compilers.