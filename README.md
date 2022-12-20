# vutu

`vutu` uses the the [loris](http://www.cerlsoundgroup.org/Loris/) library to analyze and synthesize sounds. 

## building

The following libraries are required:
- [Loris](https://github.com/madronalabs/loris)
- [madronalib](https://github.com/madronalabs/madronalib)
- [mlvg](https://github.com/madronalabs/mlvg)

Vutu can be built with CMake:
```
- mkdir build
- cd build
- cmake -GXcode .. (for Mac)
```

The libsndfile and libresample libraries are also included. Because some configuration is required to get these to work, they are compiled here from source. 

Everything is cross-platform but I'm currently working on Mac and have not been testing on Windows.

