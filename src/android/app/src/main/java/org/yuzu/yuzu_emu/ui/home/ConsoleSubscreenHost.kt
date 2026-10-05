// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.ui.home

import android.os.Bundle
import android.view.View
import android.view.ViewGroup
import androidx.annotation.IdRes
import androidx.annotation.StringRes
import androidx.core.os.bundleOf
import androidx.fragment.app.DialogFragment
import androidx.fragment.app.Fragment
import androidx.fragment.app.FragmentManager
import androidx.navigation.fragment.NavHostFragment
import com.google.android.material.appbar.AppBarLayout
import com.google.android.material.appbar.MaterialToolbar
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.features.settings.ui.SettingsSubscreen
import org.yuzu.yuzu_emu.fragments.AddonsFragment
import org.yuzu.yuzu_emu.fragments.FreedrenoSettingsFragment
import org.yuzu.yuzu_emu.fragments.GameInfoFragment
import org.yuzu.yuzu_emu.fragments.ProfileManagerFragment
import org.yuzu.yuzu_emu.model.Game

/**
 * Shows a settings subscreen (profile manager, game info, add-ons, Freedreno) inside a container
 * of the console UI instead of opening SettingsSubscreenActivity.
 *
 * The subscreen runs in its own child NavHostFragment with the regular subscreen graph, so its
 * internal navigation (e.g. profile manager -> edit user) keeps working. The console screen shows
 * its own header (title + back), so the subscreen's toolbar is hidden and its background made
 * transparent to blend with the console background.
 */
class ConsoleSubscreenHost(
    private val host: Fragment,
    @IdRes private val containerId: Int,
    private val onClosed: () -> Unit
) {
    private var navHost: NavHostFragment? = null

    val isOpen: Boolean get() = navHost != null

    init {
        // A subscreen left over from a previous view of the host (restored by the fragment
        // manager) is not tracked any more: drop it so the console screen starts clean.
        host.childFragmentManager.findFragmentByTag(TAG)?.let { stale ->
            host.childFragmentManager.beginTransaction()
                .remove(stale)
                .commitAllowingStateLoss()
        }
    }

    private val styler = object : FragmentManager.FragmentLifecycleCallbacks() {
        override fun onFragmentViewCreated(
            fm: FragmentManager,
            f: Fragment,
            v: View,
            savedInstanceState: Bundle?
        ) {
            if (f is DialogFragment) return
            v.background = null
            // Only the subscreens opened from the console UI lose their toolbar: nested screens
            // (e.g. the user editor) keep it for their own actions.
            if (f is ProfileManagerFragment || f is GameInfoFragment || f is AddonsFragment ||
                f is FreedrenoSettingsFragment
            ) {
                hideToolbars(v)
            }
        }
    }

    fun open(destination: SettingsSubscreen, game: Game?) {
        close(notify = false)
        val fragment = NavHostFragment()
        host.childFragmentManager.beginTransaction()
            .replace(containerId, fragment, TAG)
            .commitNow()
        fragment.childFragmentManager.registerFragmentLifecycleCallbacks(styler, false)

        val controller = fragment.navController
        val graph = controller.navInflater.inflate(R.navigation.settings_subscreen_navigation)
        graph.setStartDestination(startDestination(destination))
        controller.setGraph(graph, startArgs(destination, game))
        navHost = fragment
    }

    /** Back inside the subscreen: pops its own stack, or closes it. True when handled. */
    fun back(): Boolean {
        val fragment = navHost ?: return false
        if (!fragment.navController.popBackStack()) close()
        return true
    }

    fun close(notify: Boolean = true) {
        val fragment = navHost ?: return
        navHost = null
        fragment.childFragmentManager.unregisterFragmentLifecycleCallbacks(styler)
        val transaction = host.childFragmentManager.beginTransaction().remove(fragment)
        if (host.childFragmentManager.isStateSaved) {
            transaction.commitAllowingStateLoss()
        } else {
            transaction.commitNow()
        }
        if (notify) onClosed()
    }

    private fun hideToolbars(view: View) {
        when (view) {
            is AppBarLayout, is MaterialToolbar -> view.visibility = View.GONE
            is ViewGroup -> for (i in 0 until view.childCount) hideToolbars(view.getChildAt(i))
        }
    }

    companion object {
        private const val TAG = "ConsoleSubscreenHost"

        @StringRes
        fun titleOf(destination: SettingsSubscreen): Int = when (destination) {
            SettingsSubscreen.PROFILE_MANAGER -> R.string.profile_manager
            SettingsSubscreen.GAME_INFO -> R.string.info
            SettingsSubscreen.ADDONS -> R.string.add_ons
            SettingsSubscreen.FREEDRENO_SETTINGS -> R.string.freedreno_settings_title
            else -> R.string.app_name
        }

        private fun startDestination(destination: SettingsSubscreen): Int = when (destination) {
            SettingsSubscreen.PROFILE_MANAGER -> R.id.profileManagerFragment
            SettingsSubscreen.GAME_INFO -> R.id.gameInfoFragment
            SettingsSubscreen.ADDONS -> R.id.addonsFragment
            SettingsSubscreen.FREEDRENO_SETTINGS -> R.id.freedrenoSettingsFragment
            else -> throw IllegalArgumentException("Unsupported console subscreen: $destination")
        }

        private fun startArgs(destination: SettingsSubscreen, game: Game?): Bundle =
            when (destination) {
                SettingsSubscreen.FREEDRENO_SETTINGS -> bundleOf("game" to game)
                SettingsSubscreen.GAME_INFO,
                SettingsSubscreen.ADDONS -> bundleOf(
                    "game" to requireNotNull(game) { "Game is required for $destination" }
                )
                else -> Bundle()
            }
    }
}
