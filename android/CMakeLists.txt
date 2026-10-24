# The native port (native/: the decompiled game on Aurora) as an Android library for the Meta Quest
# (WakerQuest). Configure with the NDK's toolchain (android/README.md):
#
#   cmake -S android -B build/android -G Ninja \
#       -DCMAKE_TOOLCHAIN_FILE=$ANDROID_HOME/ndk/<version>/build/cmake/android.toolchain.cmake \
#       -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-33 \
#       -DCOS_RUNTIME_ASSETS=ON -DCOS_ASSETS_DIR=<output of gen_assets.sh --stubs>
#   ninja -C build/android switchwaker          # build/android/libmain.so
#
# Then android/gradlew assembleDebug packs libmain.so into the APK. The APK holds no game data: the
# asset arrays come from the player's disc at start-up (COS_RUNTIME_ASSETS, docs/RUNTIME_ASSETS.md),
# and the disc itself is copied to the headset (Android/data/<package>/files/GZLE01.iso).
#
# It follows Dusklight's Android build (the same Aurora pin) and reuses the Switch layer's disc
# reader:
#   - Dawn: Aurora's prebuilt android-aarch64 package (Vulkan);
#   - SDL 3: built from source by Aurora (the only provider it accepts on Android), static, so the
#     APK carries one library, libmain.so, which SDL's Java side loads;
#   - nod (Rust) is replaced by switch/native/nod/nod_gcn.cpp, the reader of plain GameCube disc
#     images behind nod's C API: no Rust toolchain needed;
#   - the Java side (SDL's activity, Aurora's surface, Borealis's activity and Gradle script) comes
#     from the checkouts CMake fetches; this file writes borealis-android.properties for Gradle.
# API level 33: bionic's <execinfo.h> (backtrace, for the crash report of pc_crash.cpp).
cmake_minimum_required(VERSION 3.28)
project(cos_android LANGUAGES C CXX)

if (NOT ANDROID)
    message(FATAL_ERROR "Configure with the Android NDK toolchain (android/README.md)")
endif ()
if (NOT ANDROID_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR "WakerQuest builds for arm64-v8a only (got ${ANDROID_ABI})")
endif ()
if (ANDROID_PLATFORM_LEVEL AND ANDROID_PLATFORM_LEVEL LESS 33)
    message(FATAL_ERROR "ANDROID_PLATFORM must be android-33 or newer (bionic's backtrace)")
endif ()

cmake_path(SET COS_REPO_ROOT NORMALIZE "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(COS_ANDROID_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
# Frame records everywhere: the crash report walks them.
string(APPEND CMAKE_C_FLAGS " -fno-omit-frame-pointer")
string(APPEND CMAKE_CXX_FLAGS " -fno-omit-frame-pointer")

# ---- nod: plain GameCube disc images (switch/native/nod) ---------------------------------------------
add_library(cos_android_nod STATIC "${COS_REPO_ROOT}/switch/native/nod/nod_gcn.cpp")
target_include_directories(cos_android_nod PUBLIC "${COS_REPO_ROOT}/switch/native/nod")
add_library(nod::nod ALIAS cos_android_nod)

# Aurora's providers: the existing nod::nod above, SDL from source (static), Dawn's prebuilt package.
set(AURORA_NOD_PROVIDER vendor CACHE STRING "" FORCE)
set(AURORA_NOD_LINKAGE static CACHE STRING "" FORCE)
set(AURORA_SDL3_PROVIDER vendor CACHE STRING "" FORCE)
set(AURORA_SDL3_LINKAGE static CACHE STRING "" FORCE)
set(AURORA_DAWN_PROVIDER package CACHE STRING "" FORCE)

# ---- The native port itself ---------------------------------------------------------------------------
add_subdirectory("${COS_REPO_ROOT}/native" native)

if (NOT TARGET switchwaker)
    message(FATAL_ERROR "native/ did not define switchwaker (modules or cos_sdk missing)")
endif ()

# The SDK over Aurora was written on the Mac, where plain char is signed (arm64 Darwin), as it is
# for the game (-fsigned-char, GameConfig.cmake); AArch64 ELF's is unsigned. Keep the Mac's, as the
# Switch build does.
target_compile_options(cos_sdk PRIVATE -fsigned-char)
target_compile_options(cos_sdk_gf PRIVATE -fsigned-char)

# The NDK compiles with _FORTIFY_SOURCE=2, so bionic's __*_chk abort the game where the original
# writes past an array into padding or the next member (d_a_npc_zl1's strcpy of "Zl2" into a char[3]
# did, on reaching Outset), including copies of a run-time size that no compiler warning shows. The
# game runs without FORTIFY on the Mac, Linux and the Switch: the same here, for the game units
# only (cos_game_headers: the modules and the harness that is compiled like them). Aurora, Dawn, the
# SDK and the headset layer keep it.
target_compile_options(cos_game_headers INTERFACE -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)

# pc_harness.cpp asks SDL for the app's external files directory (the data root on Android).
target_link_libraries(cos_pc PRIVATE ${AURORA_SDL3_TARGET})
# logcat for the harness's log lines (pc_harness.cpp, pc_android_log).
target_link_libraries(switchwaker PRIVATE log android)

# ---- The headset layer (xr/wq_xr.cpp): OpenXR, its own GLES context, Aurora's XR buffers ----------------
# The Khronos OpenXR loader's headers and library from its Android package on Maven Central; the APK
# gets the same library through Gradle (app/build.gradle).
set(COS_ANDROID_OPENXR_VERSION "1.1.63")
set(_cos_openxr_aar "${CMAKE_BINARY_DIR}/_deps/openxr_loader_for_android-${COS_ANDROID_OPENXR_VERSION}.aar")
set(_cos_openxr_dir "${CMAKE_BINARY_DIR}/_deps/openxr-loader")
if (NOT EXISTS "${_cos_openxr_dir}/jni/arm64-v8a/libopenxr_loader.so")
    file(DOWNLOAD
            "https://repo1.maven.org/maven2/org/khronos/openxr/openxr_loader_for_android/${COS_ANDROID_OPENXR_VERSION}/openxr_loader_for_android-${COS_ANDROID_OPENXR_VERSION}.aar"
            "${_cos_openxr_aar}"
            EXPECTED_HASH SHA256=622419d2f6741c3443a3beb4779af0764318edd01830de967f24c741ebcded73
            TLS_VERIFY ON)
    file(ARCHIVE_EXTRACT INPUT "${_cos_openxr_aar}" DESTINATION "${_cos_openxr_dir}"
            PATTERNS "jni/arm64-v8a/*" "prefab/modules/headers/include/*")
endif ()
add_library(cos_openxr_loader SHARED IMPORTED)
set_target_properties(cos_openxr_loader PROPERTIES
        IMPORTED_LOCATION "${_cos_openxr_dir}/jni/arm64-v8a/libopenxr_loader.so"
        IMPORTED_NO_SONAME OFF
        INTERFACE_INCLUDE_DIRECTORIES "${_cos_openxr_dir}/prefab/modules/headers/include")
target_sources(switchwaker PRIVATE "${COS_ANDROID_DIR}/xr/wq_xr.cpp" "${COS_ANDROID_DIR}/xr/wq_panel.cpp"
        "${COS_ANDROID_DIR}/xr/ui_canvas.cpp")
target_include_directories(switchwaker PRIVATE "${COS_ANDROID_DIR}/xr")
# the diorama's game side (native/src/pc/game_hooks/pc_vr_diorama.cpp) asks the headset layer
target_include_directories(cos_pc PRIVATE "${COS_ANDROID_DIR}/xr")
target_link_libraries(switchwaker PRIVATE cos_openxr_loader aurora::core ${AURORA_SDL3_TARGET} EGL GLESv3 nativewindow)

# ---- libmain.so for SDL's Java side ---------------------------------------------------------------------
set_target_properties(switchwaker PROPERTIES OUTPUT_NAME main LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
target_link_options(switchwaker PRIVATE "-Wl,-u,SDL_main" "-Wl,--build-id=sha1")

# The Java sources and the native library, for android/app/build.gradle (Borealis's Gradle script).
set(COS_ANDROID_BOREALIS_REPOSITORY "https://github.com/encounter/borealis.git" CACHE STRING
        "Borealis git repository (Android activity and Gradle script)")
# Dusklight's pin (extern/borealis), 2026-10-09.
set(COS_ANDROID_BOREALIS_COMMIT "4ac5e7052a8c49a122f8d57f626b5c75c5ca6968" CACHE STRING "Borealis commit")
include(FetchContent)
FetchContent_Declare(borealis
        GIT_REPOSITORY "${COS_ANDROID_BOREALIS_REPOSITORY}"
        GIT_TAG "${COS_ANDROID_BOREALIS_COMMIT}"
        SOURCE_SUBDIR do-not-build)
FetchContent_MakeAvailable(borealis)

if (NOT AURORA_SDL3_JAVA_SOURCE_DIR OR NOT AURORA_ANDROID_JAVA_SOURCE_DIR)
    message(FATAL_ERROR "Aurora did not provide its Android Java sources")
endif ()
set(COS_ANDROID_VERSION_NAME "1.0.2" CACHE STRING "APK version name")
set(COS_ANDROID_VERSION_CODE "4" CACHE STRING "APK version code")
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/borealis-android.properties" CONTENT
        "# Generated by android/CMakeLists.txt; do not edit.
borealis.nativeLibrary=$<TARGET_FILE:switchwaker>
borealis.androidAbi=${ANDROID_ABI}
borealis.androidPlatform=${ANDROID_PLATFORM}
borealis.sdlJavaDir=${AURORA_SDL3_JAVA_SOURCE_DIR}
borealis.auroraJavaDir=${AURORA_ANDROID_JAVA_SOURCE_DIR}
borealis.javaDir=${borealis_SOURCE_DIR}/platforms/android/java
borealis.dir=${borealis_SOURCE_DIR}
borealis.versionName=${COS_ANDROID_VERSION_NAME}
borealis.versionCode=${COS_ANDROID_VERSION_CODE}
")
message(STATUS "cos_android: Gradle properties in ${CMAKE_BINARY_DIR}/borealis-android.properties")
