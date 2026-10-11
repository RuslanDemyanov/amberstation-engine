// SPDX-License-Identifier: GPL-2.0-or-later
// Движок эмуляции PS1: PCSX-ReARMed + хост сессии, отдельный процесс.
plugins {
    alias(libs.plugins.android.library)
}

android {
    namespace = "com.leomze.ps1.engine"
    compileSdk {
        version = release(37)
    }

    defaultConfig {
        minSdk = 26
        ndk {
            abiFilters += "arm64-v8a"
        }
        externalNativeBuild {
            cmake {
                arguments += "-DANDROID_STL=c++_shared"
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    ndkVersion = "27.0.12077973"
}

dependencies {
    implementation(project(":engine-api"))
}
