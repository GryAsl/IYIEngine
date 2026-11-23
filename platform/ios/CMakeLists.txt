# macOS commands (run from the repository root; supply paths to the pinned XCFrameworks and HR package):
# cmake -S . -B build/ios-device -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.6 -DIYI_SHIPPING_BUILD=ON -DIYI_IOS_RAYLIB_XCFRAMEWORK=/path/raylib.xcframework -DIYI_IOS_EGL_XCFRAMEWORK=/path/libEGL.xcframework -DIYI_IOS_GLESV2_XCFRAMEWORK=/path/libGLESv2.xcframework -DIYI_IOS_PYTHON_XCFRAMEWORK=/path/Python.xcframework -DIYI_IOS_PYTHON_STDLIB=/path/python-ios -DIYI_IOS_GAME_RRES=/path/game.rres
# cmake --build build/ios-device --config Release --target IYIEngineMobile
# cmake -S . -B build/ios-simulator -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.6 -DIYI_SHIPPING_BUILD=ON -DIYI_IOS_RAYLIB_XCFRAMEWORK=/path/raylib.xcframework -DIYI_IOS_EGL_XCFRAMEWORK=/path/libEGL.xcframework -DIYI_IOS_GLESV2_XCFRAMEWORK=/path/libGLESv2.xcframework -DIYI_IOS_PYTHON_XCFRAMEWORK=/path/Python.xcframework -DIYI_IOS_PYTHON_STDLIB=/path/python-ios -DIYI_IOS_GAME_RRES=/path/game.rres
# cmake --build build/ios-simulator --config Debug --target IYIEngineMobile

enable_language(OBJCXX)
set(IYI_IOS_BUNDLE_ID "com.noksangames.humanresources" CACHE STRING "iOS application bundle identifier")
set(IYI_IOS_DEVELOPMENT_TEAM "" CACHE STRING "Apple development team (empty for unsigned builds)")
set(IYI_IOS_DEPLOYMENT_TARGET "15.6" CACHE STRING "Minimum iOS version")

add_executable(IYIEngineMobile MACOSX_BUNDLE platform/ios/main.mm)
target_link_libraries(IYIEngineMobile PRIVATE IYIEngineCore IYIEngineSim
    "${IYI_IOS_RAYLIB_XCFRAMEWORK}" "${IYI_IOS_EGL_XCFRAMEWORK}" "${IYI_IOS_GLESV2_XCFRAMEWORK}"
    "${IYI_IOS_PYTHON_XCFRAMEWORK}")
set_target_properties(IYIEngineMobile PROPERTIES
    OBJCXX_STANDARD 20
    OBJCXX_STANDARD_REQUIRED YES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/IYIEngineMobile-Info.plist"
    XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "${IYI_IOS_DEPLOYMENT_TARGET}"
    XCODE_ATTRIBUTE_SUPPORTED_PLATFORMS "iphoneos iphonesimulator"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${IYI_IOS_BUNDLE_ID}"
    XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${IYI_IOS_DEVELOPMENT_TEAM}"
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
    XCODE_EMBED_FRAMEWORKS "${IYI_IOS_PYTHON_XCFRAMEWORK};${IYI_IOS_EGL_XCFRAMEWORK};${IYI_IOS_GLESV2_XCFRAMEWORK}"
    XCODE_EMBED_FRAMEWORKS_CODE_SIGN_ON_COPY "YES")
configure_file("${CMAKE_CURRENT_LIST_DIR}/Info.plist.in" "${CMAKE_CURRENT_BINARY_DIR}/IYIEngineMobile-Info.plist" @ONLY)

file(GLOB_RECURSE _iyi_ios_stdlib CONFIGURE_DEPENDS "${IYI_IOS_PYTHON_STDLIB}/lib/python3.14/*")
foreach(_file IN LISTS _iyi_ios_stdlib)
    file(RELATIVE_PATH _relative "${IYI_IOS_PYTHON_STDLIB}/lib/python3.14" "${_file}")
    get_filename_component(_parent "${_relative}" DIRECTORY)
    set_source_files_properties("${_file}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/python/lib/python3.14/${_parent}")
endforeach()
set_source_files_properties("${IYI_IOS_GAME_RRES}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
target_sources(IYIEngineMobile PRIVATE "${IYI_IOS_GAME_RRES}" ${_iyi_ios_stdlib})
