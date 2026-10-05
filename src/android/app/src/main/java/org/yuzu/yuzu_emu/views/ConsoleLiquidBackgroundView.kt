// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.views

import android.animation.ArgbEvaluator
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.RadialGradient
import android.graphics.Shader
import android.os.SystemClock
import android.util.AttributeSet
import android.view.View
import kotlin.math.PI
import kotlin.math.cos
import kotlin.math.max
import kotlin.math.sin

/**
 * Colors shared by the console Home background and the selected tile outline, so both follow the
 * same (animated) palette. Written by [ConsoleLiquidBackgroundView], read by
 * GradientBorderCardView.
 */
object ConsolePalette {
    // Declared first: glassColor's initializer below reads it.
    private const val GLASS_ALPHA = 205

    /** Bright accents used for the selected tile outline. */
    var borderColors: IntArray = intArrayOf(Color.WHITE, Color.WHITE, Color.WHITE)
        private set

    /**
     * Palette of the last game selected on Home (null until one is). Other console screens
     * (settings) start their background with it.
     */
    var selectedBase: IntArray? = null
        private set
    var selectedAccents: IntArray? = null
        private set

    fun rememberSelection(base: IntArray, accents: IntArray) {
        selectedBase = base.clone()
        selectedAccents = accents.clone()
    }

    /** Translucent glass in the background colors (dialog cards). */
    var glassColor: Int = Color.argb(GLASS_ALPHA, 0x1B, 0x2C, 0x66)
        private set

    /** Incremented whenever [borderColors] changes, so readers can cache their shaders. */
    var version: Int = 0
        private set

    internal fun updateGlass(base: IntArray) {
        // Center of the background gradient, slightly deepened, at glass opacity.
        val hsv = FloatArray(3)
        Color.colorToHSV(base[1], hsv)
        hsv[2] *= 0.85f
        val color = Color.HSVToColor(hsv)
        glassColor = Color.argb(GLASS_ALPHA, Color.red(color), Color.green(color), Color.blue(color))
    }

    internal fun updateBorder(accents: IntArray) {
        val hsv = FloatArray(3)
        borderColors = IntArray(accents.size) { i ->
            Color.colorToHSV(accents[i], hsv)
            hsv[1] = (hsv[1] * 0.85f).coerceAtMost(0.7f)
            hsv[2] = 1f
            Color.HSVToColor(hsv)
        }
        version++
    }
}

/**
 * Animated "liquid glass" backdrop of the console Home: a base vertical gradient with large,
 * heavily blurred blobs in the accent colors of the selected game flowing across the screen,
 * under a frosted glass sheen.
 *
 * Only animates while visible on screen; color changes cross-fade.
 */
class ConsoleLiquidBackgroundView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null
) : View(context, attrs) {

    private val evaluator = ArgbEvaluator()

    private var fromBase = IntArray(BASE_COLORS)
    private var fromAccents = IntArray(ACCENT_COLORS)
    private var toBase = IntArray(BASE_COLORS)
    private var toAccents = IntArray(ACCENT_COLORS)
    private val base = IntArray(BASE_COLORS)
    private val accents = IntArray(ACCENT_COLORS)
    private var transitionStart = 0L
    private var transitioning = false

    private val basePaint = Paint()
    private val blobPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val sheenPaint = Paint()
    private val frostPaint = Paint().apply { color = Color.argb(14, 255, 255, 255) }
    private var baseShaderDirty = true

    private val startTime = SystemClock.uptimeMillis()
    private var running = false

    // Blob motion: relative center (x, y), amplitudes and periods (seconds); accents cycle.
    private val blobs = arrayOf(
        Blob(0.20f, 0.30f, 0.30f, 0.22f, 4.5f, 6f, 0.0f),
        Blob(0.80f, 0.25f, 0.28f, 0.26f, 5.5f, 4f, 1.7f),
        Blob(0.55f, 0.80f, 0.34f, 0.20f, 6.5f, 5f, 3.1f),
        Blob(0.15f, 0.85f, 0.26f, 0.24f, 4f, 7f, 4.6f),
        Blob(0.50f, 0.45f, 0.36f, 0.30f, 6f, 4.5f, 2.3f)
    )

    private class Blob(
        val cx: Float,
        val cy: Float,
        val ax: Float,
        val ay: Float,
        val px: Float,
        val py: Float,
        val phase: Float
    )

    init {
        // Start with the last game selected on Home, so every console screen shares its colors.
        setPalette(
            ConsolePalette.selectedBase ?: intArrayOf(DEFAULT_TOP, DEFAULT_CENTER, DEFAULT_BOTTOM),
            ConsolePalette.selectedAccents
                ?: intArrayOf(DEFAULT_ACCENT_1, DEFAULT_ACCENT_2, DEFAULT_ACCENT_3),
            animate = false
        )
    }

    /** Sets the target colors: 3 base gradient colors (top to bottom) and 3 accents. */
    fun setPalette(baseColors: IntArray, accentColors: IntArray, animate: Boolean = true) {
        val newBase = IntArray(BASE_COLORS) { baseColors[it.coerceAtMost(baseColors.size - 1)] }
        val newAccents = IntArray(ACCENT_COLORS) {
            accentColors[it.coerceAtMost(accentColors.size - 1)]
        }
        if (newBase.contentEquals(toBase) && newAccents.contentEquals(toAccents)) return

        if (animate) {
            fromBase = base.clone()
            fromAccents = accents.clone()
            transitionStart = SystemClock.uptimeMillis()
            transitioning = true
        } else {
            newBase.copyInto(base)
            newAccents.copyInto(accents)
            transitioning = false
            baseShaderDirty = true
            ConsolePalette.updateBorder(accents)
            ConsolePalette.updateGlass(base)
        }
        toBase = newBase
        toAccents = newAccents
        invalidate()
    }

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        updateRunning()
    }

    override fun onDetachedFromWindow() {
        super.onDetachedFromWindow()
        running = false
    }

    override fun onWindowVisibilityChanged(visibility: Int) {
        super.onWindowVisibilityChanged(visibility)
        updateRunning()
    }

    override fun onVisibilityAggregated(isVisible: Boolean) {
        super.onVisibilityAggregated(isVisible)
        updateRunning()
    }

    private fun updateRunning() {
        val shouldRun = isAttachedToWindow && windowVisibility == VISIBLE && isShown
        if (shouldRun && !running) {
            running = true
            invalidate()
        } else if (!shouldRun) {
            running = false
        }
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        baseShaderDirty = true
    }

    private fun stepTransition(now: Long) {
        if (!transitioning) return
        val fraction = ((now - transitionStart) / TRANSITION_MS.toFloat()).coerceIn(0f, 1f)
        val eased = fraction * fraction * (3f - 2f * fraction)
        for (i in 0 until BASE_COLORS) {
            base[i] = evaluator.evaluate(eased, fromBase[i], toBase[i]) as Int
        }
        for (i in 0 until ACCENT_COLORS) {
            accents[i] = evaluator.evaluate(eased, fromAccents[i], toAccents[i]) as Int
        }
        baseShaderDirty = true
        ConsolePalette.updateBorder(accents)
        ConsolePalette.updateGlass(base)
        if (fraction >= 1f) transitioning = false
    }

    override fun onDraw(canvas: Canvas) {
        val w = width.toFloat()
        val h = height.toFloat()
        if (w <= 0f || h <= 0f) return

        val now = SystemClock.uptimeMillis()
        stepTransition(now)
        val t = (now - startTime) / 1000f

        if (baseShaderDirty) {
            basePaint.shader = LinearGradient(
                0f, 0f, 0f, h,
                base, floatArrayOf(0f, 0.45f, 1f),
                Shader.TileMode.CLAMP
            )
            baseShaderDirty = false
        }
        canvas.drawRect(0f, 0f, w, h, basePaint)

        // Liquid blobs: wide, soft radial gradients in the accent colors flowing on Lissajous
        // paths. The long multi-stop falloff keeps them blurred into each other.
        val radius = max(w, h) * 0.75f
        for (i in blobs.indices) {
            val blob = blobs[i]
            val x = w * (blob.cx + blob.ax * sin(2f * PI.toFloat() * t / blob.px + blob.phase))
            val y = h * (blob.cy + blob.ay * cos(2f * PI.toFloat() * t / blob.py + blob.phase))
            val pulse = 1f + 0.12f * sin(2f * PI.toFloat() * t / (blob.px * 0.6f) + blob.phase)
            val r = radius * pulse
            val color = accents[i % ACCENT_COLORS]
            blobPaint.shader = RadialGradient(
                x, y, r,
                intArrayOf(
                    withAlpha(color, BLOB_ALPHA),
                    withAlpha(color, BLOB_ALPHA * 3 / 5),
                    withAlpha(color, BLOB_ALPHA / 4),
                    withAlpha(color, BLOB_ALPHA / 14),
                    0
                ),
                floatArrayOf(0f, 0.25f, 0.55f, 0.8f, 1f),
                Shader.TileMode.CLAMP
            )
            canvas.drawCircle(x, y, r, blobPaint)
        }

        // Frosted glass: light veil plus a soft diagonal sheen sweeping slowly across.
        canvas.drawRect(0f, 0f, w, h, frostPaint)
        val sweep = ((t / SHEEN_PERIOD_S) % 1f) * 2f - 0.5f
        val sx = w * sweep
        sheenPaint.shader = LinearGradient(
            sx - w * 0.35f, 0f, sx + w * 0.35f, h,
            intArrayOf(0, Color.argb(22, 255, 255, 255), 0),
            floatArrayOf(0f, 0.5f, 1f),
            Shader.TileMode.CLAMP
        )
        canvas.drawRect(0f, 0f, w, h, sheenPaint)

        if (running) postInvalidateDelayed(FRAME_DELAY_MS)
    }

    private fun withAlpha(color: Int, alpha: Int): Int =
        Color.argb(alpha, Color.red(color), Color.green(color), Color.blue(color))

    private companion object {
        const val BASE_COLORS = 3
        const val ACCENT_COLORS = 3
        const val TRANSITION_MS = 600L
        const val FRAME_DELAY_MS = 33L
        const val BLOB_ALPHA = 140
        const val SHEEN_PERIOD_S = 6f

        val DEFAULT_TOP = Color.rgb(0x0A, 0x12, 0x38)
        val DEFAULT_CENTER = Color.rgb(0x1B, 0x2C, 0x66)
        val DEFAULT_BOTTOM = Color.rgb(0x5B, 0x6F, 0x98)
        val DEFAULT_ACCENT_1 = Color.rgb(0x3A, 0x5B, 0xD9)
        val DEFAULT_ACCENT_2 = Color.rgb(0x6A, 0x4C, 0xC9)
        val DEFAULT_ACCENT_3 = Color.rgb(0x2E, 0x8C, 0xC7)
    }
}
