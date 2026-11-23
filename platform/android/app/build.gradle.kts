// IYIEngine Android shell: a NativeActivity subclass loading libiyigame.so,
// which is the engine built by the root CMakeLists.txt with raylib's Android platform.
// Per-game values are Gradle properties (see ../gradle.properties); tools/release.py passes them.
import java.security.MessageDigest

plugins {
    id("com.android.application")
}

fun prop(name: String): String? = (findProperty(name) as String?)?.takeIf { it.isNotBlank() }
fun required(name: String): String = prop(name) ?: throw GradleException("Missing -P$name (see gradle.properties)")

val engineDir = file("../../..")
val pythonPrefix = file(prop("iyi.pythonPrefix") ?: "$engineDir/build/deps/python-android/prefix")
val pythonVersion = "3.14"
val generatedInclude = required("iyi.generatedInclude")   // mobile only loads the packaged game.rres: always a shipping build
val gameRres = prop("iyi.gameRres")
val playGamesAppId = prop("iyi.playGamesAppId")   // set = Play Games on; absent = no APP_ID meta-data at all

android {
    buildFeatures { resValues = true; aidl = true }
    namespace = "com.iyiengine.shell"   // code namespace only; the store identity is applicationId
    compileSdk = 36
    ndkVersion = "28.2.13676358"         // r28+: 16 KB ELF alignment by default

    defaultConfig {
        applicationId = required("iyi.applicationId")
        manifestPlaceholders["iyiIcon"] = if (prop("iyi.iconRes") != null) "@mipmap/iyi_launcher" else "@android:drawable/sym_def_app_icon"
        minSdk = 24                       // OS floor; never raise silently (docs/mobile/plan/00 item 4)
        targetSdk = 36                    // Play policy 2026-11; separate from minSdk
        versionCode = (prop("iyi.versionCode") ?: "1").toInt()
        versionName = prop("iyi.versionName") ?: "0.1"
        resValue("string", "app_name", prop("iyi.appName") ?: "IYIEngine")
        // Google Play bridge (engine.play): empty = off. release.json fills these (see docs/spec/mobile.md).
        resValue("string", "iyi_play_achievement_ids", prop("iyi.playAchievementIds") ?: "")
        resValue("string", "iyi_license_public_key", prop("iyi.licensePublicKey") ?: "")
        resValue("string", "iyi_games_app_id", playGamesAppId ?: "")
        ndk { abiFilters += "arm64-v8a" }
        externalNativeBuild {
            cmake {
                targets += "iyigame"
                arguments += listOfNotNull(
                    "-DIYI_PYTHON_TARGET_ROOT=${pythonPrefix.invariantSeparatorsPath}",
                    "-DIYI_STEAM=OFF",
                    "-DIYI_BUILD_TESTS=OFF",
                    "-DIYI_BUILD_TOOLS=OFF",
                    "-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON",
                    "-DANDROID_STL=c++_shared",
                    "-DIYI_SHIPPING_BUILD=ON",
                    "-DIYI_GENERATED_INCLUDE_DIR=${file(generatedInclude).invariantSeparatorsPath}",
                )
            }
        }
    }

    prop("iyi.iconRes")?.let { sourceSets.getByName("main").res.srcDir(file(it)) }

    externalNativeBuild {
        cmake {
            path = file("$engineDir/CMakeLists.txt")
            version = "3.31.6"
        }
    }

    signingConfigs {
        // Release key only from the environment; never committed (docs/mobile/plan/05).
        create("release") {
            System.getenv("IYI_ANDROID_KEYSTORE")?.let { storeFile = file(it) }
            storePassword = System.getenv("IYI_ANDROID_KEYSTORE_PASSWORD")
            keyAlias = System.getenv("IYI_ANDROID_KEY_ALIAS")
            keyPassword = System.getenv("IYI_ANDROID_KEY_PASSWORD")
        }
    }
    buildTypes {
        release {
            isMinifyEnabled = false
            // No release key: debug-signed for local device QA only (Play rejects a debug-signed upload).
            signingConfig = signingConfigs.getByName(if (System.getenv("IYI_ANDROID_KEYSTORE") != null) "release" else "debug")
            isDebuggable = System.getenv("IYI_ANDROID_DEBUGGABLE") != null  // device QA: adb run-as can reach the saves
        }
    }

    // game.rres is read in place through AAsset (random access): it must stay uncompressed in the APK.
    androidResources {
        noCompress += listOf("rres")
        // AAPT's default pattern drops "<dir>_*" folders, which removes stdlib packages (__phello__, _pyrepl).
        ignoreAssetsPattern = "iyi-dont-ignore-anything"   // same trick as the CPython testbed
    }
    packaging { jniLibs { useLegacyPackaging = false } }
}

// Google Play bridge (engine.play); pins live in tools/mobile/dependency-lock.json. LVL itself has no
// Maven artifact: src/main/aidl + License.java are the vendored minimal client.
dependencies {
    implementation("com.google.android.gms:play-services-games-v2:22.1.0")
    implementation("com.google.android.play:review:2.0.2")
}

// Python stdlib (official CPython Android layout) + game.rres as generated assets.
// The engine copies assets/python/** to internal storage when manifest.txt's hash changes.
abstract class GenerateTask : Sync() {
    @get:OutputDirectory abstract val outputDir: DirectoryProperty
}

val stdlibExcludes = listOf(
    "**/__pycache__/**", "test/**", "idlelib/**", "tkinter/**", "turtledemo/**",
    "ensurepip/**", "site-packages/**", "config-*/**", "**/*.a",
)

androidComponents.onVariants { variant ->
    // Play Games reads com.google.android.gms.games.APP_ID from the manifest: add it only when
    // release.json has an id, so an unconfigured build has no games meta-data at all.
    if (playGamesAppId != null) {
        variant.sources.manifests.addStaticManifestFile("src/play/AndroidManifest.xml")
    }
    val cap = variant.name.replaceFirstChar(Char::uppercase)
    val assets = tasks.register<GenerateTask>("generate${cap}IyiAssets") {
        into(outputDir)
        into("python/lib/python$pythonVersion") {
            from("$pythonPrefix/lib/python$pythonVersion")
            exclude(stdlibExcludes)
        }
        if (gameRres != null) from(gameRres) { rename { "game.rres" } }
        doLast {
            val root = outputDir.get().asFile.resolve("python")
            val files = root.walkTopDown().filter { it.isFile && it.name != "manifest.txt" }
                .map { it.relativeTo(root).invariantSeparatorsPath }.sorted().toList()
            val md = MessageDigest.getInstance("SHA-256")
            for (rel in files) { md.update(rel.toByteArray()); md.update(root.resolve(rel).readBytes()) }
            val hash = md.digest().joinToString("") { "%02x".format(it) }
            root.resolve("manifest.txt").writeText((listOf(hash) + files).joinToString("\n") + "\n")
        }
    }
    variant.sources.assets!!.addGeneratedSourceDirectory(assets, GenerateTask::outputDir)

    val libs = tasks.register<GenerateTask>("generate${cap}IyiJniLibs") {
        into(outputDir)
        into("arm64-v8a") {
            from("$pythonPrefix/lib")
            include("libpython*.so", "lib*_python.so")
        }
    }
    variant.sources.jniLibs!!.addGeneratedSourceDirectory(libs, GenerateTask::outputDir)
}
