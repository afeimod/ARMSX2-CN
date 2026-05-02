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
        ndk {
            abiFilters.add("arm64-v8a")
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            // Sign release with the debug keystore so it's installable on-device
            // without a separate signing config. NOT for distribution — the debug
            // keystore is well-known and not secure for Play Store uploads.
            // Replace with a real release signingConfig before publishing.
            signingConfig = signingConfigs.getByName("debug")
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
            externalNativeBuild {
                cmake {
                    arguments += "-DANDROID=true"
                    arguments += "-DANDROID_STL=c++_static"
                    arguments += "-DCMAKE_BUILD_TYPE=Release"
                    arguments += "-DCMAKE_C_FLAGS=-O3 -g"
                    arguments += "-DCMAKE_CXX_FLAGS=-O3 -g"
                }
            }
        }
        debug {
            // Keep PCSX2_DEBUG/VIXL_DEBUG defines (via CMAKE_BUILD_TYPE=Debug)
            // but compile at -O3 to match release's
            // codegen. -O0 was exposing a JIT-adjacent crash in MGS2 that
            // -O3 release didn't hit, which narrows the cause to stack/
            // uninitialised-local fragility rather than the debug defines.
            // -ffp-contract=off was previously kept for VU1 bit-exactness
            // but only affects C/C++ FP code, not JIT-emitted FMUL/FADD.
            // Removing it lets the compiler fuse a*b+c → FMADD in counters,
            // GS software renderer, SPU2 audio mixing, IPU, VIF unpack —
            // significant FP-heavy paths. JIT'd VU FMAC semantics are
            // unaffected because the recompiler emits explicit Fmul+Fadd.
            externalNativeBuild {
                cmake {
                    arguments += "-DANDROID=true"
                    arguments += "-DANDROID_STL=c++_static"
                    arguments += "-DCMAKE_BUILD_TYPE=Debug"
                    arguments += "-DCMAKE_C_FLAGS=-O3 -g"
                    arguments += "-DCMAKE_CXX_FLAGS=-O3 -g"
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

// Android Studio's "Build > Clean Project" runs the `clean` task, but AGP
// leaves `app/.cxx/` (the CMake/Ninja workspace) in place. Stale .cxx state
// can lead to ghost builds — old object files linking against newer headers,
// or vice versa. Wire `cleanCxx` into `clean` so the native build workspace
// gets wiped too.
tasks.register<Delete>("cleanCxx") {
    delete(layout.projectDirectory.dir(".cxx"))
}
tasks.named("clean") {
    dependsOn("cleanCxx")
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
    implementation(libs.androidx.compose.ui.tooling.preview)
    implementation(libs.androidx.compose.foundation)
    implementation(libs.androidx.documentfile)
    implementation(libs.coil.compose)

    testImplementation(libs.junit)
    androidTestImplementation(libs.androidx.junit)
    androidTestImplementation(libs.androidx.espresso.core)
    debugImplementation(libs.androidx.compose.ui.tooling)
}