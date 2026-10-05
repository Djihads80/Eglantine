// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.yuzu.yuzu_emu.views

import android.content.Context
import android.graphics.*
import android.os.SystemClock
import android.util.AttributeSet
import com.google.android.material.card.MaterialCardView
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.features.settings.model.IntSetting

class GradientBorderCardView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0
) : MaterialCardView(context, attrs, defStyleAttr) {

    private val borderPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 6f
    }

    private val borderPath = Path()
    private val borderRect = RectF()
    private var showGradientBorder = false
    private var isEdenTheme = false

    // Console outline: sweep gradient in the Home palette colors (ConsolePalette), rotating.
    private val consoleShaderMatrix = Matrix()
    private var consolePaletteVersion = -1

    /**
     * Console Home style: animated outline in the background palette colors on the
     * selected/focused tile instead of the theme gradient. Toggled by CarouselRecyclerView when
     * its consoleStyle is enabled.
     */
    var consoleHighlight: Boolean = false
        set(value) {
            if (field == value) return
            field = value
            // Keep the artwork inside the rounded tile (no corners poking past the outline).
            clipToOutline = value
            if (width > 0 && height > 0) applyBorderStyle(width, height)
            invalidate()
        }

    init {
        setWillNotDraw(false)
        updateThemeState()
    }

    private fun updateBorderState() {
        val shouldShow = isPressed || isFocused || isSelected
        if (showGradientBorder != shouldShow) {
            showGradientBorder = shouldShow
            invalidate()
        }
    }

    private fun updateThemeState() {
        val themeIndex = IntSetting.STATIC_THEME_COLOR.getInt(false)
        isEdenTheme = themeIndex == 0
        invalidate()
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        applyBorderStyle(w, h)
    }

    private fun applyBorderStyle(w: Int, h: Int) {
        borderPaint.strokeWidth = if (consoleHighlight) {
            CONSOLE_STROKE_DP * resources.displayMetrics.density
        } else {
            DEFAULT_STROKE_PX
        }

        // Update border style based on theme
        if (consoleHighlight) {
            consolePaletteVersion = -1
            borderPaint.color = Color.WHITE
        } else if (isEdenTheme) {
            // Gradient for Eden theme
            borderPaint.shader = LinearGradient(
                0f,
                0f,
                w.toFloat(),
                h.toFloat(),
                context.getColor(R.color.eden_border_gradient_start),
                context.getColor(R.color.eden_border_gradient_end),
                Shader.TileMode.CLAMP
            )
        } else {
            // Solid color for other themes
            borderPaint.shader = null
            val typedValue = android.util.TypedValue()
            context.theme.resolveAttribute(
                com.google.android.material.R.attr.colorPrimary,
                typedValue,
                true
            )
            borderPaint.color = typedValue.data
        }

        // Update border rect with padding for stroke
        val halfStroke = borderPaint.strokeWidth / 2
        borderRect.set(
            halfStroke,
            halfStroke,
            w - halfStroke,
            h - halfStroke
        )

        // Update path with rounded corners. The console outline is inset by half its stroke, so
        // its radius shrinks by the same amount to stay concentric with the tile corners.
        val pathRadius = if (consoleHighlight) (radius - halfStroke).coerceAtLeast(0f) else radius
        borderPath.reset()
        borderPath.addRoundRect(
            borderRect,
            pathRadius,
            pathRadius,
            Path.Direction.CW
        )
    }

    override fun onFocusChanged(gainFocus: Boolean, direction: Int, previouslyFocusedRect: Rect?) {
        super.onFocusChanged(gainFocus, direction, previouslyFocusedRect)
        updateBorderState()
    }

    override fun setSelected(selected: Boolean) {
        super.setSelected(selected)
        updateBorderState()
    }

    override fun setPressed(pressed: Boolean) {
        super.setPressed(pressed)
        updateBorderState()
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        if (showGradientBorder && !consoleHighlight) {
            canvas.drawPath(borderPath, borderPaint)
        }
    }

    override fun dispatchDraw(canvas: Canvas) {
        super.dispatchDraw(canvas)
        // Console tiles are edge-to-edge artwork, so the outline must be drawn above the children.
        if (showGradientBorder && consoleHighlight) {
            updateConsoleShader()
            canvas.drawPath(borderPath, borderPaint)
            if (isAttachedToWindow) postInvalidateDelayed(CONSOLE_FRAME_DELAY_MS)
        }
    }

    private fun updateConsoleShader() {
        val cx = width / 2f
        val cy = height / 2f
        if (consolePaletteVersion != ConsolePalette.version || borderPaint.shader == null) {
            val palette = ConsolePalette.borderColors
            // Close the loop so the sweep has no seam.
            val colors = IntArray(palette.size + 1) { palette[it % palette.size] }
            borderPaint.shader = SweepGradient(cx, cy, colors, null)
            consolePaletteVersion = ConsolePalette.version
        }
        val angle = (SystemClock.uptimeMillis() % CONSOLE_ROTATION_MS) * 360f / CONSOLE_ROTATION_MS
        consoleShaderMatrix.setRotate(angle, cx, cy)
        borderPaint.shader.setLocalMatrix(consoleShaderMatrix)
    }

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        updateThemeState()
    }

    private companion object {
        const val DEFAULT_STROKE_PX = 6f
        const val CONSOLE_STROKE_DP = 3.72f
        const val CONSOLE_ROTATION_MS = 4000L
        const val CONSOLE_FRAME_DELAY_MS = 33L
    }
}
