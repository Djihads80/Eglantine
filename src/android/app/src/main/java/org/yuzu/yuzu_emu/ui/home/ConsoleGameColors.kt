// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.ui.home

import android.content.res.Resources
import android.graphics.Bitmap
import android.graphics.Color
import androidx.lifecycle.LifecycleOwner
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.model.Game
import org.yuzu.yuzu_emu.utils.GameIconUtils

/**
 * Colors of the console background for a game, taken from its icon. Shared by Home and the game
 * screen so both show the same animated background.
 */
object ConsoleGameColors {
    /** Base gradient (top, center, bottom) and the accents for the liquid blobs. */
    class Palette(val base: IntArray, val accents: IntArray)

    private const val ACCENT_COUNT = 3
    private const val HUE_BUCKETS = 12
    private const val SAMPLE_SIZE = 24

    private val defaultAccents = intArrayOf(
        Color.rgb(0x3A, 0x5B, 0xD9),
        Color.rgb(0x6A, 0x4C, 0xC9),
        Color.rgb(0x2E, 0x8C, 0xC7)
    )

    // Palettes by game path, kept for the process lifetime.
    private val cache = HashMap<String, Palette>()

    /** Palette used when no game is selected. */
    fun defaultPalette(res: Resources) = Palette(
        intArrayOf(
            res.getColor(R.color.console_home_bg_top, null),
            res.getColor(R.color.console_home_bg_center, null),
            res.getColor(R.color.console_home_bg_bottom, null)
        ),
        defaultAccents.clone()
    )

    fun cached(game: Game): Palette? = cache[game.path]

    /** Loads the game's icon and computes its palette (cached). Null if the icon fails. */
    suspend fun load(owner: LifecycleOwner, game: Game): Palette? {
        cache[game.path]?.let { return it }
        val palette = try {
            val icon = GameIconUtils.getGameIcon(owner, game)
            withContext(Dispatchers.Default) { paletteOf(accentColors(icon)) }
        } catch (e: Exception) {
            null
        } ?: return null
        cache[game.path] = palette
        return palette
    }

    private fun paletteOf(accents: IntArray) = Palette(
        gradientFor(accents[0]),
        IntArray(accents.size) { liquidAccent(accents[it]) }
    )

    /**
     * Main colors of the icon: pixels are grouped by hue (weighted by saturation and brightness)
     * and the heaviest groups are averaged, strongest first. Missing slots (few colors, or a mostly
     * grey icon) are filled with hue shifts of the main color.
     */
    private fun accentColors(icon: Bitmap): IntArray {
        val sample = Bitmap.createScaledBitmap(icon, SAMPLE_SIZE, SAMPLE_SIZE, true)
        val pixels = IntArray(SAMPLE_SIZE * SAMPLE_SIZE)
        sample.getPixels(pixels, 0, SAMPLE_SIZE, 0, 0, SAMPLE_SIZE, SAMPLE_SIZE)
        if (sample !== icon) sample.recycle()

        val weights = FloatArray(HUE_BUCKETS)
        val sums = Array(HUE_BUCKETS) { FloatArray(3) }
        val total = FloatArray(3)
        val hsv = FloatArray(3)
        var count = 0
        for (pixel in pixels) {
            if (Color.alpha(pixel) < 128) continue
            val r = Color.red(pixel).toFloat()
            val g = Color.green(pixel).toFloat()
            val b = Color.blue(pixel).toFloat()
            total[0] += r
            total[1] += g
            total[2] += b
            count++
            Color.colorToHSV(pixel, hsv)
            val weight = hsv[1] * hsv[2]
            if (weight < 0.08f) continue
            val bucket = ((hsv[0] / 360f) * HUE_BUCKETS).toInt().coerceIn(0, HUE_BUCKETS - 1)
            weights[bucket] += weight
            sums[bucket][0] += r * weight
            sums[bucket][1] += g * weight
            sums[bucket][2] += b * weight
        }
        if (count == 0) return defaultAccents.clone()

        val minWeight = count * 0.03f
        val found = weights.indices
            .filter { weights[it] >= minWeight }
            .sortedByDescending { weights[it] }
            .take(ACCENT_COUNT)
            .map { bucket ->
                val weight = weights[bucket]
                Color.rgb(
                    (sums[bucket][0] / weight).toInt(),
                    (sums[bucket][1] / weight).toInt(),
                    (sums[bucket][2] / weight).toInt()
                )
            }
        val main = found.firstOrNull() ?: Color.rgb(
            (total[0] / count).toInt(),
            (total[1] / count).toInt(),
            (total[2] / count).toInt()
        )
        return IntArray(ACCENT_COUNT) { i ->
            found.getOrNull(i) ?: shiftHue(main, if (i % 2 == 1) 28f else -28f)
        }
    }

    private fun shiftHue(color: Int, degrees: Float): Int {
        val hsv = FloatArray(3)
        Color.colorToHSV(color, hsv)
        hsv[0] = (hsv[0] + degrees + 360f) % 360f
        return Color.HSVToColor(hsv)
    }

    /** Accent tuned for the liquid blobs: vivid but not glaring under white text. */
    private fun liquidAccent(color: Int): Int {
        val hsv = FloatArray(3)
        Color.colorToHSV(color, hsv)
        hsv[1] = hsv[1].coerceIn(0.35f, 0.9f)
        hsv[2] = hsv[2].coerceIn(0.45f, 0.8f)
        return Color.HSVToColor(hsv)
    }

    /** Dark-to-light gradient in the hue of [color], dark enough to keep white text readable. */
    private fun gradientFor(color: Int): IntArray {
        val hsv = FloatArray(3)
        Color.colorToHSV(color, hsv)
        val hue = hsv[0]
        val saturation = hsv[1]
        return intArrayOf(
            Color.HSVToColor(floatArrayOf(hue, (saturation * 0.9f).coerceAtMost(0.85f), 0.14f)),
            Color.HSVToColor(floatArrayOf(hue, (saturation * 0.8f).coerceAtMost(0.75f), 0.26f)),
            Color.HSVToColor(floatArrayOf(hue, (saturation * 0.5f).coerceAtMost(0.5f), 0.42f))
        )
    }
}
