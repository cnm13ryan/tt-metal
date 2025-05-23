# Data Movement Test Suite

This test suite addresses the functionality and performance (i.e. bandwidth) of various data movement scenarios.

## Tests in the Test Suite
1. **DRAM Unary**: Transactions between DRAM and a single Tensix core.
2. **One to One**: Transactions between two Tensix cores. (In Progress)

## Running Tests
Before running any tests, build the repo with tests: ```./build_metal.sh --build-tests```
Then, to run the whole test suite execute the following command:
```
TT_METAL_SLOW_DISPATCH_MODE=1 ./build/test/tt_metal/unit_tests_data_movement
```

To run a single test, add a gtest filter with the name of the test. Example:
```
TT_METAL_SLOW_DISPATCH_MODE=1 ./build/test/tt_metal/unit_tests_data_movement gtest_filter="*TensixDataMovementDRAMInterleavedPacketSizes*"
```

## Adding Tests
Follow these steps to add new tests to this test suite.

1. Create a new directory with a descriptive name for the test.
    - **Example:** `./dram_unary`
2. In this directory, create the c++ test file with a filename that starts with "test_".
    - **Example:** `./dram_unary/test_unary_dram.cpp`
3. Write your test in this file and place the kernels you use within this test in "kernels" directory.
    - **Example:** `./dram_unary/kernels/reader_unary.cpp`
4. Create a README file within the test directory that describes:
    1. What your test does,
    2. What the test parameters are,
    3. And what different test cases are implemented.
5. In the `CMakeLists.txt` file, add your test path in the `set(UNIT_TESTS_DATA_MOVEMENT_SRC ... )` call.
    - **Example:** `${CMAKE_CURRENT_SOURCE_DIR}/dram_unary/test_unary_dram.cpp`

**Note:** Make sure the tests pass by building and running as above.
