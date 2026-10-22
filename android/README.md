# WakerQuest: the native port on Android (Meta Quest)

`android/` builds the native port (`native/`) as `libmain.so` and packs it into an APK with
Borealis's Gradle script, as Dusklight does. The APK holds no game data: build with
`COS_RUNTIME_ASSETS=ON` against the stub headers of `native/tools/gen_assets.sh --stubs`, and copy your
own disc to the headset.

## Requirements

Android SDK with platform 35, NDK r29 (29.0.14206865), CMake 3.28 or newer with Ninja (the SDK's
cmake 3.31.6 package has both), JDK 17 or newer, git.

## Build (any host: Windows, Linux, macOS)

```sh
native/tools/gen_assets.sh --stubs --out "$PWD/build/android-assets/GZLE01"   # no disc needed
cmake -S android -B build/android -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=$ANDROID_HOME/ndk/29.0.14206865/build/cmake/android.toolchain.cmake \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-33 -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCOS_RUNTIME_ASSETS=ON -DCOS_ASSETS_DIR=$PWD/build/android-assets/GZLE01
ninja -C build/android switchwaker
cd android && ANDROID_NDK_VERSION=29.0.14206865 ./gradlew :app:assembleDebug
adb install -r app/build/outputs/apk/debug/app-arm64-v8a-debug.apk
```

## The disc and the data

Start the app once (it creates its folder), then copy your uncompressed `GZLE01.iso` (USA, revision 0):

```sh
adb push GZLE01.iso /sdcard/Android/data/io.github.wakerquest/files/GZLE01.iso
```

Saves, settings and caches go to `files/user/`, the log to `files/logs/switchwaker.log`.
