// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.ui.home

import android.graphics.Bitmap
import android.util.LruCache
import androidx.lifecycle.LifecycleOwner
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.yuzu.yuzu_emu.model.Game
import org.yuzu.yuzu_emu.utils.GameIconUtils

/**
 * Produces the soft "blurred artwork" used as the console Home backdrop.
 *
 * Instead of running a real blur, the game icon (already decoded and memory-cached by Coil in
 * [GameIconUtils]) is downscaled to a tiny bitmap; the ImageView then upscales it with bilinear
 * filtering, which looks like a strong blur at almost zero cost. Results are kept in a small LRU
 * (each entry is ~2 KB), so moving back and forth through the library never re-decodes anything.
 */
object GameBackdropCache {
    private const val BACKDROP_SIZE_PX = 24
    private const val MAX_ENTRIES = 64

    private val cache = LruCache<String, Bitmap>(MAX_ENTRIES)

    private fun Game.backdropKey(): String = "$path|$version"

    fun peek(game: Game): Bitmap? = cache.get(game.backdropKey())

    suspend fun load(owner: LifecycleOwner, game: Game): Bitmap? {
        val key = game.backdropKey()
        cache.get(key)?.let { return it }

        val icon = try {
            GameIconUtils.getGameIcon(owner, game)
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            return null
        }

        val small = withContext(Dispatchers.Default) {
            Bitmap.createScaledBitmap(icon, BACKDROP_SIZE_PX, BACKDROP_SIZE_PX, true)
        }
        cache.put(key, small)
        return small
    }

    fun clear() = cache.evictAll()
}
