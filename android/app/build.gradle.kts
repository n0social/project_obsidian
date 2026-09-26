plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.obsidian.client"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.obsidian.client"
        minSdk = 26
        targetSdk = 35
        versionCode = 1
        versionName = "0.2.2-emulator"

        // Device (arm64) + Windows emulator (x86_64)
        ndk {
            abiFilters += listOf("arm64-v8a", "x86_64")
        }

        externalNativeBuild {
            cmake {
                cppFlags += listOf("-std=c++20", "-fexceptions", "-frtti")
                val cmakeArgs = mutableListOf(
                    "-DANDROID_STL=c++_shared",
                    "-DOBSIDIAN_ANDROID=ON",
                    "-DWOWEE_DIR=${rootProject.projectDir.parentFile.resolve("WoWee").invariantSeparatorsPath}",
                )
                val vcpkgRoot = System.getenv("VCPKG_ROOT")
                    ?: listOf(
                        "${System.getProperty("user.home")}/vcpkg",
                        "C:/Users/donav/vcpkg",
                    ).firstOrNull { file(it).isDirectory }
                if (vcpkgRoot != null) {
                    cmakeArgs += "-DVCPKG_ROOT=${file(vcpkgRoot).invariantSeparatorsPath}"
                }
                arguments += cmakeArgs
            }
        }

        buildConfigField("String", "ENGINE_NAME", "\"project_obsidian\"")
    }

    buildFeatures {
        buildConfig = true
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    ndkVersion = "30.0.15729638"

    packaging {
        jniLibs {
            useLegacyPackaging = true
        }
    }
}

dependencies {
    // SAF folder pick for classic client / extracted Data import.
    implementation("androidx.documentfile:documentfile:1.0.1")
}
