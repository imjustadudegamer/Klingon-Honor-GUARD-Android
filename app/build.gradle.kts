import java.io.FileInputStream
import java.util.Properties

plugins {
    id("com.android.application")
}

// The native engine sources (UE1, SDL2, OpenAL Soft) and all Android/KHG
// patches are committed directly under src/main/cpp. There is no download or
// codegen step — CMake builds the tree as-is. See docs/ARCHITECTURE.md.

// Release signing is configured only if a local keystore.properties exists
// (it is gitignored and never committed). Without it, the release build is
// produced unsigned and you can sign it yourself.
val keystorePropertiesFile = rootProject.file("keystore.properties")
val hasKeystore = keystorePropertiesFile.exists()
val keystoreProperties = Properties().apply {
    if (hasKeystore) FileInputStream(keystorePropertiesFile).use { load(it) }
}

android {
    namespace = "com.khg.android"
    compileSdk = 36

    signingConfigs {
        if (hasKeystore) {
            create("release") {
                storeFile = file(keystoreProperties["storeFile"] as String)
                storePassword = keystoreProperties["storePassword"] as String
                keyAlias = keystoreProperties["keyAlias"] as String
                keyPassword = keystoreProperties["keyPassword"] as String
            }
        }
    }

    buildTypes {
        getByName("release") {
            isMinifyEnabled = false
            if (hasKeystore) {
                signingConfig = signingConfigs.getByName("release")
            }
        }
    }

    tasks.withType<org.gradle.api.tasks.compile.JavaCompile>().configureEach {
        options.compilerArgs.addAll(listOf("-Xlint:none"))
    }

    buildFeatures {
        buildConfig = true
    }

    // namespace is com.khg.android: it is the Java/JNI package (native symbol names
    // like Java_com_khg_android_* depend on it). It matches the installed applicationId.
    defaultConfig {
        applicationId = "com.khg.android"
        minSdk = 23
        targetSdk = 36
        versionCode = 1
        versionName = "0.6.1"

        ndk {
            abiFilters += listOf("armeabi-v7a")
        }

        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DANDROID_STL=c++_shared",
                    "-DANDROID_PLATFORM=android-23"
                )
                cppFlags += listOf("-std=c++17", "-fexceptions", "-frtti")
            }
        }
    }

    ndkVersion = "27.0.12077973"

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    sourceSets {
        getByName("main") {
            java.srcDirs(
                "src/main/java",
                "src/main/cpp/thirdparty/SDL2/android-project/app/src/main/java"
            )
            assets.srcDir("src/main/assets")
        }
    }

    packaging {
        jniLibs {
            useLegacyPackaging = true
        }
        resources {
            excludes += setOf("/META-INF/{AL2.0,LGPL2.1}")
        }
    }
}
