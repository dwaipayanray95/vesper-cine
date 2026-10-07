plugins {
    id("com.android.application")
    // The Flutter Gradle Plugin must be applied after the Android and Kotlin Gradle plugins.
    id("dev.flutter.flutter-gradle-plugin")
    id("com.google.gms.google-services")
    id("com.google.firebase.crashlytics")
}

// google-services.json is the Firebase project's config. The committed one is
// a placeholder (project "vesper-placeholder") so every build compiles; replace
// it with the file downloaded from the real Firebase project. Only then are
// mapping / native symbol files uploaded (the upload would fail otherwise).
val firebaseIsReal = !file("google-services.json").readText().contains("vesper-placeholder")

android {
    namespace = "com.theawesomeray.vespercine"
    compileSdk = flutter.compileSdkVersion
    ndkVersion = flutter.ndkVersion

    firebaseCrashlytics {
        mappingFileUploadEnabled = firebaseIsReal
        nativeSymbolUploadEnabled = firebaseIsReal
    }

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
        // AdMob app id from the repository variable VESPER_ADMOB_APP_ID; without it
        // Google's public test id is used (test ads only, no revenue).
        manifestPlaceholders["admobAppId"] =
            System.getenv("VESPER_ADMOB_APP_ID")?.takeIf { it.isNotBlank() }
                ?: "ca-app-pub-3940256099942544~3347511713"

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
    // It's a tester key committed to the repo on purpose. Redefining the
    // standard "debug" config covers debug, profile and release builds.
    // The Play Store bundle is signed with a separate private "upload" key
    // that only exists as GitHub Actions secrets (see docs/PLAY_RELEASE.md):
    // it is used when VESPER_UPLOAD_KEYSTORE is set, otherwise release builds
    // fall back to the tester key so local builds keep working.
    signingConfigs {
        getByName("debug") {
            storeFile = file("vesper-dev.jks")
            storePassword = "vesperdev"
            keyAlias = "vesper"
            keyPassword = "vesperdev"
        }
        val uploadStore = System.getenv("VESPER_UPLOAD_KEYSTORE")
        if (!uploadStore.isNullOrEmpty()) {
            create("upload") {
                storeFile = file(uploadStore)
                storePassword = System.getenv("VESPER_UPLOAD_STORE_PASSWORD")
                keyAlias = System.getenv("VESPER_UPLOAD_KEY_ALIAS")
                keyPassword = System.getenv("VESPER_UPLOAD_KEY_PASSWORD")
            }
        }
    }

    buildTypes {
        release {
            signingConfig = signingConfigs.findByName("upload")
                ?: signingConfigs.getByName("debug")
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

dependencies {
    // Native (C++) crash reports: the Flutter plugin only brings the Java part.
    // Keep the BoM in step with firebase_core's FirebaseSDKVersion.
    implementation(platform("com.google.firebase:firebase-bom:34.19.0"))
    implementation("com.google.firebase:firebase-crashlytics-ndk")
}
