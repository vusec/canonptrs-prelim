# canonptrs-prelim

As described in the paper "Canon Pointers: Conditional Delta Tagging for Compatible Buffer Overflow Protection" at PLAS 2025.

## Building LLVM
```
cd llvm-project
mkdir build
cd build
cmake -DLLVM_ENABLE_PROJECTS="clang;lld" -DLLVM_ENABLE_RUNTIMES="compiler-rt" -DCMAKE_BUILD_TYPE=Release -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=On -DLLVM_PARALLEL_LINK_JOBS=3 -DLLVM_TARGETS_TO_BUILD=X86 -DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON -DCLANG_ENABLE_STATIC_ANALYZER=OFF -DCLANG_ENABLE_ARCMT=OFF ../llvm
ninja
```

## Testing Canon Pointer instrumentation
```
./build/bin/clang test.c -fsanitize=canonptr -o test
```
Then inspect the resulting assembly with objdump, or look at the IR with:
```
./build/bin/clang test.c -fsanitize=canonptr -S -emit-llvm -o -
```
