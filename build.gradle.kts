// BOSS · 根构建脚本
//
// ⚠️ AGP 9.x 内置了 Kotlin（built-in Kotlin），**不能再**声明 kotlin.android 插件。
//    声明了就是这次这个错：
//      InvalidPluginException: An exception occurred applying plugin request
//      [id: 'org.jetbrains.kotlin.android', version: '2.3.0']
//      → The 'org.jetbrains.kotlin.android' plugin is no longer required
//        for Kotlin support since AGP 9.0.
//    报错说"应用插件时异常"，但没说"这个插件不该声明"——
//    只看第一行会往"版本不对"上想，实际是"插件根本不该出现"。
//
//    所以这里只留两个：AGP 本体 + Compose 编译器插件。
buildscript {
    repositories {
        google()
        mavenCentral()
    }
    dependencies {
        // 为什么要有这一段：
        //   AGP 9.1.1 内置的是 KGP 2.2.10，而我们要用 Kotlin 2.3.0。
        //   内置 Kotlin 只会**向上**补到 2.2.10，不会自己升到 2.3.0；
        //   而 Compose 编译器插件的版本必须与**实际使用的 Kotlin 版本一致**
        //   （Compose 编译器随 Kotlin 一起发布）。
        //   不在这里显式拉 KGP 的话，组合是「KGP 2.2.10 + compose 插件 2.3.0」，
        //   版本对不齐会报 Compose/Kotlin 不兼容。
        //
        // ⚠️ 版本目录在 buildscript{} 里读不到，所以这里必须写死，
        //    并手动与 gradle/libs.versions.toml 的 kotlin = "2.3.0" 保持一致。
        //    改 Kotlin 版本时**两处都要改**。
        classpath("org.jetbrains.kotlin:kotlin-gradle-plugin:2.3.0")
    }
}

plugins {
    alias(libs.plugins.android.application) apply false
    // 不再声明 kotlin.android —— 见文件头
    alias(libs.plugins.kotlin.compose) apply false
}
