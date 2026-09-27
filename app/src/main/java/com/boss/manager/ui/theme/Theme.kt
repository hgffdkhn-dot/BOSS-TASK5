package com.boss.manager.ui.theme

import android.os.Build
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.ExperimentalMaterial3ExpressiveApi
import androidx.compose.material3.MaterialExpressiveTheme
import androidx.compose.material3.MotionScheme
import androidx.compose.material3.Shapes
import androidx.compose.material3.Typography
import androidx.compose.material3.dynamicDarkColorScheme
import androidx.compose.material3.dynamicLightColorScheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.foundation.shape.RoundedCornerShape

/**
 * BOSS 主题 = Material 3 Expressive（Android 16 的原生视觉语言）。
 *
 * 为什么是 Expressive 而不是普通 MaterialTheme：
 *   它是 Android 16 系统 UI 的同款语言——动态取色、弹簧动效、更大的圆角、
 *   强调型排版。BOSS 主打"日用"，界面跟系统长得像本身就是隐蔽的一部分：
 *   一个和系统不同代的界面，比任何特征都显眼。
 *
 * 三个决定：
 *
 * 1. **动态配色优先**（Android 12+）。取色来自壁纸（Monet），不写死品牌色。
 *    这既符合原生观感，也契合"低痕迹"——不引入一个可识别的 BOSS 色。
 *    12 以下回退到下面这组中性色，而不是某个鲜艳的品牌色。
 *
 * 2. **MotionScheme.expressive()**。弹簧动效，会略微过冲再回弹。
 *    Material 官方建议把它用在主要交互上；工具类界面才用 standard()。
 *    BOSS 的操作（授权裁决、挂载摘除）需要明确的"我动了"反馈，所以用 expressive。
 *
 * 3. **不碰 Emphasized 排版变体**。displayLargeEmphasized 那一族仍是 alpha 里
 *    相对活跃的部分，而标准 Typography() 已经足够。这里的原则是：
 *    alpha 依赖只吃到真正需要的那几个 API，其余一律用稳定面。
 */
@OptIn(ExperimentalMaterial3ExpressiveApi::class)
@Composable
fun BossTheme(
    darkTheme: Boolean = isSystemInDarkTheme(),
    // 动态配色只在 Android 12+ 有；关掉它就会用下面那组回退色。
    dynamicColor: Boolean = true,
    content: @Composable () -> Unit,
) {
    val colorScheme = when {
        dynamicColor && Build.VERSION.SDK_INT >= Build.VERSION_CODES.S -> {
            val context = LocalContext.current
            if (darkTheme) dynamicDarkColorScheme(context) else dynamicLightColorScheme(context)
        }
        darkTheme -> BossDark
        else -> BossLight
    }

    MaterialExpressiveTheme(
        colorScheme = colorScheme,
        motionScheme = MotionScheme.expressive(),
        shapes = BossShapes,
        typography = Typography(),
        content = content,
    )
}

/* ---------------- 回退配色（Android 12 以下，或用户关掉动态取色）----------------
 *
 * 刻意用低饱和的中性色：BOSS 不该有自己的"招牌色"。
 * 一个能在截图里被认出来的颜色，对主打隐蔽的产品是负资产。 */
private val BossLight = lightColorScheme(
    primary = Color(0xFF0B5FA5),
    onPrimary = Color(0xFFFFFFFF),
    primaryContainer = Color(0xFFD3E5FF),
    onPrimaryContainer = Color(0xFF001D33),
    secondary = Color(0xFF1E7A55),
    onSecondary = Color(0xFFFFFFFF),
    secondaryContainer = Color(0xFFCDEBD9),
    onSecondaryContainer = Color(0xFF002114),
    error = Color(0xFFB3261E),
    onError = Color(0xFFFFFFFF),
    errorContainer = Color(0xFFF9DEDC),
    onErrorContainer = Color(0xFF410E0B),
    surface = Color(0xFFFBFDFD),
    onSurface = Color(0xFF191C1D),
    surfaceVariant = Color(0xFFDCE4E8),
    onSurfaceVariant = Color(0xFF40484C),
    outline = Color(0xFF70787D),
)

private val BossDark = darkColorScheme(
    primary = Color(0xFF9ECAFF),
    onPrimary = Color(0xFF003258),
    primaryContainer = Color(0xFF004A7F),
    onPrimaryContainer = Color(0xFFD3E5FF),
    secondary = Color(0xFFA6D8BF),
    onSecondary = Color(0xFF003823),
    secondaryContainer = Color(0xFF005239),
    onSecondaryContainer = Color(0xFFCDEBD9),
    error = Color(0xFFFFB4AB),
    onError = Color(0xFF690005),
    errorContainer = Color(0xFF93000A),
    onErrorContainer = Color(0xFFFFDAD6),
    surface = Color(0xFF101415),
    onSurface = Color(0xFFE1E2E5),
    surfaceVariant = Color(0xFF40484C),
    onSurfaceVariant = Color(0xFFC0C8CC),
    outline = Color(0xFF8A9297),
)

/**
 * Expressive 的形状：比标准 M3 更大的圆角，并给出 largeIncreased / extraLargeIncreased
 * 这两档——它们是 Expressive 新增的，卡片、工具栏这类大容器用它才有那代观感。
 */
private val BossShapes = Shapes(
    extraSmall = RoundedCornerShape(4.dp),
    small = RoundedCornerShape(8.dp),
    medium = RoundedCornerShape(16.dp),
    large = RoundedCornerShape(24.dp),
    extraLarge = RoundedCornerShape(32.dp),
    largeIncreased = RoundedCornerShape(28.dp),
    extraLargeIncreased = RoundedCornerShape(40.dp),
)
