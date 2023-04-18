# gptj.cpp

Port of [GPT-J](https://en.wikipedia.org/wiki/GPT-J) model in C/C++. Adapted from [ggerganov/ggml/examples](https://github.com/ggerganov/ggml/tree/master/examples).

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

On Windows, the generated `.dll` files will be located at:

```
build\bin\Release\gptj.dll
build\bin\Release\ggml.dll
```

## License

[MIT](https://github.com/marella/gptj.cpp/blob/main/LICENSE)
