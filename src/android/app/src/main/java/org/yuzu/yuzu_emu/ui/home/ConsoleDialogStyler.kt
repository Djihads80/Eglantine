// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.ui.home

import android.app.Dialog
import android.content.res.ColorStateList
import android.graphics.Color
import android.graphics.drawable.InsetDrawable
import androidx.fragment.app.DialogFragment
import androidx.fragment.app.Fragment
import androidx.fragment.app.FragmentManager
import com.google.android.material.shape.MaterialShapeDrawable
import org.yuzu.yuzu_emu.views.ConsolePalette

/**
 * Tints dialogs with the console's current (game) palette: the dialog card becomes translucent
 * glass in the colors of the animated background, with a thin light outline.
 *
 * Dialog colors in the theme are static, so this is applied when a dialog is shown: automatically
 * for every DialogFragment of the activity ([fragmentCallbacks]) and through [style] for dialogs
 * built and shown directly.
 */
object ConsoleDialogStyler {
    private const val OUTLINE_ALPHA = 64
    private const val OUTLINE_DP = 1f

    val fragmentCallbacks = object : FragmentManager.FragmentLifecycleCallbacks() {
        override fun onFragmentStarted(fm: FragmentManager, f: Fragment) {
            (f as? DialogFragment)?.dialog?.let { style(it) }
        }
    }

    /** Returns [dialog] so it can be chained after `show()`. */
    fun <T : Dialog> style(dialog: T): T {
        val window = dialog.window ?: return dialog
        val background = window.decorView.background
        val shape = ((background as? InsetDrawable)?.drawable ?: background) as? MaterialShapeDrawable
            ?: return dialog
        val density = window.context.resources.displayMetrics.density
        shape.fillColor = ColorStateList.valueOf(ConsolePalette.glassColor)
        shape.setStroke(
            OUTLINE_DP * density,
            ColorStateList.valueOf(Color.argb(OUTLINE_ALPHA, 255, 255, 255))
        )
        return dialog
    }
}
