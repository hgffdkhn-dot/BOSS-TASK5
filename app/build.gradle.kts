// BOSS · App 模块
//
// 两个不能改的地方：
//   1. applicationId 必须与 daemon 侧的 BOSS_MANAGER_PKG 一致（com.boss.manager）。
//      manager 身份判定要拿内核给的 uid 再对一遍包名；包名对不上，
//      表现是"App 里所有功能都显示被拒绝"，且不报任何错。
//   2. minSdk 26 对齐 payload/manifest.json 的 min_api。
plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.kotlin.compose)
}

android {
    namespace = "com.boss.manager"
    compileSdk = 36          // Android 16：edge-to-edge 与预测式返回在这个级别上是强制的

    defaultConfig {
        applicationId = "com.boss.manager"
        minSdk = 26
        targetSdk = 36
        versionCode = 1
        versionName = "0.1.0"

        ndk {
            // 与 build/build-ndk.sh 的四 ABI 对齐，别多也别少
            abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64", "x86")
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildFeatures {
        compose = true
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"))
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
        // Expressive 的 API 还在 alpha，opt-in 警告会刷屏。
        // 全局开关放在这里，而不是每个文件各写一遍 @OptIn。
        freeCompilerArgs += listOf(
            "-opt-in=androidx.compose.material3.ExperimentalMaterial3ExpressiveApi",
        )
    }

    packaging {
        resources.excludes += "/META-INF/{AL2.0,LGPL2.1}"
    }
}

dependencies {
    implementation(platform(libs.compose.bom))

    implementation(libs.compose.ui)
    implementation(libs.compose.foundation)
    implementation(libs.compose.material3)          // 版本锁死见 libs.versions.toml
    implementation(libs.compose.ui.tooling.preview)
    debugImplementation(libs.compose.ui.tooling)

    implementation(libs.androidx.activity.compose)
    implementation(libs.androidx.core.splashscreen)
    implementation(libs.androidx.lifecycle.runtime.compose)
    implementation(libs.androidx.lifecycle.viewmodel.compose)

    // 单元测试：协议布局断言有 C 那一份（tools/ipc_layout_test.c），
    // 这里跑的是 Kotlin 侧解析器——上游改了 printf 格式，红的是这些用例。
    testImplementation(kotlin("test"))
}
