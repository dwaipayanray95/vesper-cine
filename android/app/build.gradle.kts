plugins {
    id("com.android.application")
    // The Flutter Gradle Plugin must be applied after the Android and Kotlin Gradle plugins.
    id("dev.flutter.flutter-gradle-plugin")
}

android {
    namespace = "com.theawesomeray.vespercine"
    compileSdk = flutter.compileSdkVersion
    ndkVersion = flutter.ndkVersion

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    defaultConfig {
        applicationId = "com.theawesomeray.vespercine"
        minSdk = 30
        targetSdk = flutter.targetSdkVersion
        versionCode = flutter.versionCode
        versionName = flutter.versionName

        externalNativeBuild {
            cmake {
                cppFlags += "-std=c++20"
                arguments += listOf("-DANDROID_STL=c++_shared")
            }
        }
        ndk {
            abiFilters += listOf("arm64-v8a", "x86_64")
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    // One fixed key for every build (local and GitHub Actions), so a new APK
    // installs over the previous one instead of failing with a signature
    // mismatch (each CI machine otherwise generates its own random debug key).
    // It's a tester key committed to the repo on purpose; a Play Store release
    // would need a private key kept out of the repository. Redefining the
    // standard "debug" config covers debug, profile and release builds.
    signingConfigs {
        getByName("debug") {
            storeFile = file("vesper-dev.jks")
            storePassword = "vesperdev"
            keyAlias = "vesper"
            keyPassword = "vesperdev"
        }
    }

    buildTypes {
        release {
            signingConfig = signingConfigs.getByName("debug")
        }
    }
}

kotlin {
    compilerOptions {
        jvmTarget = org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17
    }
}

flutter {
    source = "../.."
}
