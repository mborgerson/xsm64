A super minimal rendering backend for Xbox

Integrating with an upstream project might look like:
```bash
git clone <upstream project repo> main
cd main
git clone <this repo> xsm64
git am xsm64/patch/*
cp -ruT xsm64/src src
git clone --recurse-submodules -j8 https://github.com/XboxDev/nxdk.git
make TARGET_XBOX=1 bin/default.xbe
```
