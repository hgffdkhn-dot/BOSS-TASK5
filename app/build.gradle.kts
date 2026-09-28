// BOSS · App 模块
//
// 两个不能改的地方：
//   1. applicationId 必须与 daemon 侧的 BOSS_MANAGER_PKG 一致（com.boss.manager）。
//      manager 身份判定要拿内核给的 uid 再对一遍包名；包名对不上，
//      表现是"App 里所有功能都显示被拒绝"，且不报任何错。
//   2. minSdk 26 对齐 payload/manifest.json 的 min_api。
plugins {
    alias(libs.plugins.android.application)
    /* ⚠️ 这里**不能**有 kotlin.android。
     * AGP 9.0 起 Kotlin 编译内置在 AGP 里，再声明 org.jetbrains.kotlin.android
     * 会直接冲突：
     *   InvalidPluginException: ... applying plugin request
     *   [id: 'org.jetbrains.kotlin.android', version: '2.3.0']
     *   → The 'org.jetbrains.kotlin.android' plugin is no longer required
     *     for Kotlin support since AGP 9.0.
     * 报错只说"应用插件时异常"，不提"不该声明"——只看第一行会往版本上想，
     * 实际是插件本身要删掉。Kotlin 版本改在根 build.gradle.kts 的 buildscript 里。 */
    alias(libs.plugins.kotlin.compose)
}

android {
    namespace = "com.boss.manager"

    /* compileSdk 为什么是 37 而不是 36：
     *   不是我们想用 API 37 的新接口，是**被依赖逼的**——
     *   Compose BOM 2026.08.00 把 ui/foundation 锁在 1.12.0，而 1.12.0 的
     *   AAR 元数据里写着 "requires compileSdk 37 / requires AGP 9.1.0"。
     *   于是 checkDebugAarMetadata 直接硬失败（29 个问题），不是警告。
     *
     *   ⚠️ 如果报 "Failed to find target with hash string 'android-37'"：
     *      API 37 在某些 SDK 版本里只以次要版本形式发行（android-37.0 / 37.1），
     *      这时需要在下面补一行 compileSdkMinor = 0（要求 AGP ≥ 9.1.0）。
     *      我们没默认写它，因为多数环境下写了反而找不到目标。
     *
     *   另：本地必须先装好 android-37 平台 + SDK Build Tools 36.0.0，
     *   否则 AGP 连目标都找不到。 */
    compileSdk = 37

    /* compileSdkMinor 为什么必须写：
     *   API 37 只以**次要版本**形式发行（android-37.0 / android-37.1），
     *   没有不带后缀的 android-37。AGP 找的是精确 hash——
     *   不写这行它就去找 'android-37'，必然报
     *     Failed to find target with hash string 'android-37'
     *   而这句报错非常容易被误读成"平台没装上"，其实装了，只是号码不对。
     *
     * ⚠️ 这里写 0 只是默认值。**装的是 37.1 就必须写 1**：
     *   compileSdkMinor=0 → android-37.0；=1 → android-37.1。
     *   号码不一致报的是同一句话，看不出差别。
     *   本地自查：bash tools/check_android_toolchain.sh（会列出实际装了哪些 37.x）
     *   CI：apk job 里有个步骤会自动把这里改成实际装上的那个号码。
     *   要求 AGP ≥ 9.1.0。 */
    compileSdkMinor = 0

    defaultConfig {
        applicationId = "com.boss.manager"
        minSdk = 26
        /* targetSdk 刻意留在 36，不跟着 compileSdk 走。
         * 这三者是独立的（错误提示原文也这么说了）：
         *   compileSdk —— 能用哪些新 API（被依赖逼着升）
         *   targetSdk  —— 采不采用新的运行时行为（Android 16 的 edge-to-edge
         *                  与预测式返回在 36 上已经是强制的，够用了）
         *   minSdk     —— 能装到哪些设备上（26，对齐 payload 的 min_api）
         * 把 targetSdk 一起推到 37 会引入一批新的运行时行为变更，
         * 而这些变更我们一行都没验过。别顺手改。 */
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



    packaging {
        resources.excludes += "/META-INF/{AL2.0,LGPL2.1}"
    }
}

/* 顶层 kotlin{} 而不是 android{ kotlinOptions{} }：
 *   AGP 9 的内置 Kotlin 下，编译器选项搬到顶层 kotlin.compilerOptions{}。
 *   android.kotlinOptions{} 是旧写法（官方迁移文档明确要求改）。
 *
 * jvmTarget 默认跟随 compileOptions.targetCompatibility，这里显式写 17 只为清楚。
 *
 * opt-in 这一项不能省：Material 3 Expressive 的 API 还在 alpha，
 * 不开全局 opt-in 的话每个用到它的文件都得手写 @OptIn，漏一个就编译失败。 */
kotlin {
    compilerOptions {
        jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17)
        optIn.add("androidx.compose.material3.ExperimentalMaterial3ExpressiveApi")
    }
}

dependencies {
    implementation(platform(libs.compose.bom))

    implementation(libs.compose.ui)
    implementation(libs.compose.foundation)
    implementation(libs.compose.material3)          // 版本锁死见 libs.versions.toml
    // 图标：必须用 extended 而不是 core。
    // Home 在 core 里，但 Extension / Security / VisibilityOff 只在 extended。
    // 挂了 core 的表现是 "Unresolved reference: Extension"，
    // 而它不会提示"换 extended 就好"，只会让人以为图标名写错了。
    // 代价：extended 是全量图标集，APK 会大几百 KB。
    // 想瘦身就把这三个换成 core 里确实存在的图标，然后改挂 core。
    implementation(libs.compose.material.icons.extended)
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
