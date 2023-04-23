# gptj.cpp [![build](https://github.com/marella/gptj.cpp/actions/workflows/build.yml/badge.svg)](https://github.com/marella/gptj.cpp/actions/workflows/build.yml)

Port of [GPT-J](https://en.wikipedia.org/wiki/GPT-J) model in C/C++. Adapted from [ggerganov/ggml/examples](https://github.com/ggerganov/ggml/tree/master/examples).

> For Python bindings, see [marella/gpt4all-j](https://github.com/marella/gpt4all-j)

## Usage

### Download

```sh
git clone --recurse-submodules https://github.com/marella/gptj.cpp
cd gptj.cpp
```

### Build

```sh
mkdir build
cd build
cmake ..
cmake --build . --config Release
```

On Linux, the generated `.so` files will be located at:

```
build/src/libgptj.so
build/ggml/src/libggml.so
```

On macOS, the generated `.dylib` files will be located at:

```
build/src/libgptj.dylib
build/ggml/src/libggml.dylib
```

On Windows, the generated `.dll` files will be located at:

```
build\bin\Release\gptj.dll
build\bin\Release\ggml.dll
```

## License

[MIT](https://github.com/marella/gptj.cpp/blob/main/LICENSE)
