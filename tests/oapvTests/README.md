# System tests for OpenAPV in ffmpeg

# Build
```
cmake -S . -B build
cmake --build build
```
# Run 
```
cd build
ctest
```

or 

```
./oapv_tests --gtest_output=xml:test_results.xml
```