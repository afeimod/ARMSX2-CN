plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.compose.compiler)
}

android {
    namespace = "com.armsx2"
    compileSdk {
        version = release(36) {
            minorApiLevel = 1
        }
    }

    defaultConfig {
        applicationId = "com.armsx2"
        minSdk = 26
        targetSdk = 36
        versionCode = 1
        versionName = "1.0"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        externalNativeBuild {
            cmake {
                // pass flags to clang
                arguments += "-DANDROID=true"
                arguments += "-DANDROID_STL=c++_static"
                arguments += "-DCMAKE_BUILD_TYPE=Release"
                arguments += "-DCMAKE_C_FLAGS=-O2 -ffp-contract=off"
                arguments += "-DCMAKE_CXX_FLAGS=-O2 -ffp-contract=off"
            }
        }
        ndk {
            abiFilters.add("arm64-v8a")
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
            externalNativeBuild {
                cmake {
                    arguments += "-DANDROID=true"
                    arguments += "-DANDROID_STL=c++_static"
                    arguments += "-DCMAKE_BUILD_TYPE=Release"
                    arguments += "-DCMAKE_C_FLAGS=-O3 -g -ffp-contract=off"
                    arguments += "-DCMAKE_CXX_FLAGS=-O3 -g -ffp-contract=off"
                }
            }
        }
        debug {
            // Keep PCSX2_DEBUG/VIXL_DEBUG defines (via CMAKE_BUILD_TYPE=Debug)
            // but compile at -O3 to match release's
            // codegen. -O0 was exposing a JIT-adjacent crash in MGS2 that
            // -O3 release didn't hit, which narrows the cause to stack/
            // uninitialised-local fragility rather than the debug defines.
            // -ffp-contract=off is kept because the VU1 JIT's bit-exact
            // float semantics depend on separate FMUL+FADD (not fused FMA).
            externalNativeBuild {
                cmake {
                    arguments += "-DANDROID=true"
                    arguments += "-DANDROID_STL=c++_static"
                    arguments += "-DCMAKE_BUILD_TYPE=Debug"
                    arguments += "-DCMAKE_C_FLAGS=-O3 -g -ffp-contract=off"
                    arguments += "-DCMAKE_CXX_FLAGS=-O3 -g -ffp-contract=off"
                }
            }
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
        }
    }
}

dependencies {
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.appcompat)
    implementation(libs.material)

    //AndroidX Compose
    implementation(libs.androidx.activity.compose)
    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.material3)
    implementation(libs.composeIcons.fontAwesome)
    implementation(libs.composeIcons.lineAwesome)

    implementation(libs.kotlin.reflect)

    testImplementation(libs.junit)
    androidTestImplementation(libs.androidx.junit)
    androidTestImplementation(libs.androidx.espresso.core)
}