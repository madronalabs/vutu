# utu-view

`utu-view` uses the utu library and the [loris](http://www.cerlsoundgroup.org/Loris/) library to analyze and synthesize sounds. 

## building

`utu-view` requires a C++17 capable compiler, tested compilers include:

| compiler | version | platform | os |
| -------- | ------- | -------- | -- |
| gcc | 10.2.1 20210110 | aarch64-linux-gnu | Debian Bullseye |
| clang | Apple clang version 13.0.0 | arm64-apple-darwin21.2.0 | Monterey |
| clang | Apple clang version 13.0.0 | x86_64-apple-darwin20.6.0 | Big Sur |

### dependencies

`utu-view` requires the utu, mlvg-basic and madronalib libraries. The easiest way to install these is currently from source. 


### building

once dependencies are installed building `utu-view` itself can be done as follows:

```
git clone ssh://git@github.com/madronalabs/utu-view.git
cd utu-view
git submodule update --init --depth 1
mkdir build
cd build
cmake ..
cmake --build .

```
