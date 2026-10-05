// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.ui.home

import android.graphics.Color
import android.os.Bundle
import android.view.View
import android.view.ViewGroup
import androidx.fragment.app.DialogFragment
import androidx.fragment.app.Fragment
import androidx.fragment.app.FragmentManager
import androidx.navigation.fragment.NavHostFragment
import com.google.android.material.appbar.AppBarLayout
import com.google.android.material.appbar.CollapsingToolbarLayout
import com.google.android.material.appbar.MaterialToolbar

/**
 * Makes every screen of a settings activity (about, drivers, installers, ...) show the console's
 * animated background placed behind the activity's fragment container: the screen's own opaque
 * background and its top bar backgrounds/scrims are cleared. Cards and lists keep their style.
 */
object ConsoleScreenStyler {
    val fragmentCallbacks = object : FragmentManager.FragmentLifecycleCallbacks() {
        override fun onFragmentViewCreated(
            fm: FragmentManager,
            f: Fragment,
            v: View,
            savedInstanceState: Bundle?
        ) {
            if (f is DialogFragment || f is NavHostFragment) return
            v.background = null
            clearBars(v)
        }
    }

    private fun clearBars(view: View) {
        when (view) {
            is AppBarLayout -> view.setBackgroundColor(Color.TRANSPARENT)
            is CollapsingToolbarLayout -> {
                view.setBackgroundColor(Color.TRANSPARENT)
                view.setContentScrimColor(Color.TRANSPARENT)
                view.setStatusBarScrimColor(Color.TRANSPARENT)
            }
            is MaterialToolbar -> view.setBackgroundColor(Color.TRANSPARENT)
        }
        if (view is ViewGroup) {
            for (i in 0 until view.childCount) clearBars(view.getChildAt(i))
        }
    }
}
