// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.ui.home

import android.content.Context
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView

/**
 * Horizontal LinearLayoutManager for the console Home carousel.
 *
 * In console style the neighbour tiles are drawn scaled down and pulled towards the center, so
 * items whose layout slot is outside the viewport are still visually on screen. Laying out a bit
 * of extra space on each side keeps the row filled up to the screen edges.
 */
class ConsoleCarouselLayoutManager(context: Context) :
    LinearLayoutManager(context, RecyclerView.HORIZONTAL, false) {

    override fun calculateExtraLayoutSpace(state: RecyclerView.State, extraLayoutSpace: IntArray) {
        val extra = width / 2
        extraLayoutSpace[0] = extra
        extraLayoutSpace[1] = extra
    }
}
