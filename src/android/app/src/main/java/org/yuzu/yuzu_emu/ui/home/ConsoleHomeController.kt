// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.ui.home

import android.graphics.Bitmap
import android.net.Uri
import android.view.InputDevice
import android.view.KeyEvent
import android.view.View
import android.view.ViewGroup
import android.widget.ImageView
import android.widget.TextView
import android.widget.Toast
import androidx.core.os.bundleOf
import androidx.fragment.app.Fragment
import androidx.lifecycle.lifecycleScope
import androidx.navigation.fragment.findNavController
import androidx.recyclerview.widget.RecyclerView
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import org.yuzu.yuzu_emu.HomeNavigationDirections
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.adapters.GameAdapter
import org.yuzu.yuzu_emu.features.settings.ui.SettingsSubscreen
import org.yuzu.yuzu_emu.model.Game
import org.yuzu.yuzu_emu.ui.CarouselRecyclerView
import org.yuzu.yuzu_emu.ui.main.MainActivity
import java.util.Locale

/**
 * Presentation layer of the console-style (PS4-like) landscape Home.
 *
 * It only *decorates* the existing games screen: GamesFragment, GameAdapter, GamesViewModel and the
 * launch flow are reused untouched. This class:
 *  - switches the carousel into its flat console style,
 *  - follows the centered game and shows its details (title, developer, playtime, version, format),
 *  - cross-fades a blurred artwork backdrop for the selected game,
 *  - wires the Play / Options / Info actions to the existing card click / long-click handlers,
 *  - wires the bottom dock and the profile button to existing navigation destinations,
 *  - adds gamepad shortcuts (X = options, Y = search).
 *
 * Nothing here runs continuously: work happens only on selection changes, and every animation is a
 * short one-shot ViewPropertyAnimator.
 */
class ConsoleHomeController private constructor(
    private val fragment: Fragment,
    private val root: View,
    private val carousel: CarouselRecyclerView,
    private val adapter: GameAdapter
) {
    private val backdropA: ImageView = root.findViewById(R.id.console_backdrop_a)
    private val backdropB: ImageView = root.findViewById(R.id.console_backdrop_b)
    private val details: View = root.findViewById(R.id.console_details)
    private val gameTitle: TextView = root.findViewById(R.id.console_game_title)
    private val gameMeta: TextView = root.findViewById(R.id.console_game_meta)
    private val playButton: View = root.findViewById(R.id.console_play_button)
    private val optionsButton: View = root.findViewById(R.id.console_options_button)
    private val infoButton: View = root.findViewById(R.id.console_info_button)
    private val hints: View = root.findViewById(R.id.console_hints)

    private val res = root.resources
    private val backdropAlpha = res.getFraction(R.fraction.console_home_backdrop_alpha, 1, 1)
    private val backdropDelayMs = res.getInteger(R.integer.console_home_backdrop_delay_ms).toLong()
    private val backdropFadeMs = res.getInteger(R.integer.console_home_backdrop_fade_ms).toLong()
    private val detailsFadeMs = res.getInteger(R.integer.console_home_details_fade_ms).toLong()
    private val detailsShiftPx = res.getDimension(R.dimen.console_home_details_shift)
    private val minDetailsMarginPx = res.getDimension(R.dimen.spacing_large)

    private var active = false
    private var released = false
    private var selectedGame: Game? = null
    private var pendingPosition = RecyclerView.NO_POSITION
    private var backdropJob: Job? = null
    private var frontIsA = false
    private var backdropGame: Game? = null

    private val applySelectionRunnable = Runnable { applySelection() }

    private val carouselLayoutListener =
        View.OnLayoutChangeListener { _, _, _, _, _, _, _, _, _ -> alignDetailsWithCard() }

    companion object {
        /**
         * Attaches the console Home if the current layout provides it (res/layout-land/fragment_games.xml).
         * Returns null for layouts without console views (e.g. portrait), leaving behaviour unchanged.
         */
        fun attachIfSupported(
            fragment: Fragment,
            root: View,
            carousel: CarouselRecyclerView?,
            adapter: GameAdapter
        ): ConsoleHomeController? {
            if (carousel == null || root.findViewById<View?>(R.id.console_details) == null) {
                return null
            }
            return ConsoleHomeController(fragment, root, carousel, adapter).also { it.bind() }
        }
    }

    private fun bind() {
        carousel.onCenteredPositionChanged = { position, _ -> onCenteredPosition(position) }
        carousel.keyInterceptor = ::handleCarouselKey
        carousel.addOnLayoutChangeListener(carouselLayoutListener)
        details.addOnLayoutChangeListener(carouselLayoutListener)

        playButton.setOnClickListener { launchSelected() }
        optionsButton.setOnClickListener { openOptionsForSelected() }
        infoButton.setOnClickListener { openInfoForSelected() }

        // UP from the action row always returns to the selected tile (not to whichever card
        // happens to be geometrically above the button).
        val backToCarousel = View.OnKeyListener { _, keyCode, event ->
            if (keyCode == KeyEvent.KEYCODE_DPAD_UP && event.action == KeyEvent.ACTION_DOWN) {
                carousel.focusCenteredCard()
                true
            } else {
                false
            }
        }
        playButton.setOnKeyListener(backToCarousel)
        optionsButton.setOnKeyListener(backToCarousel)
        infoButton.setOnKeyListener(backToCarousel)

        bindDock()

        root.findViewById<View?>(R.id.console_profile_button)?.setOnClickListener {
            navigateToSubscreen(SettingsSubscreen.PROFILE_MANAGER)
        }

        updateHintsVisibility()
    }

    private fun bindDock() {
        root.findViewById<View>(R.id.console_dock_library).setOnClickListener {
            carousel.focusCenteredCard()
        }
        root.findViewById<View>(R.id.console_dock_install).setOnClickListener {
            navigateToSubscreen(SettingsSubscreen.INSTALLABLE)
        }
        root.findViewById<View>(R.id.console_dock_settings).setOnClickListener {
            fragment.findNavController().navigate(R.id.action_gamesFragment_to_homeSettingsFragment)
        }
        root.findViewById<View>(R.id.console_dock_applets).setOnClickListener {
            if (NativeLibrary.isFirmwareAvailable()) {
                navigateToSubscreen(SettingsSubscreen.APPLET_LAUNCHER)
            } else {
                Toast.makeText(
                    fragment.requireContext(),
                    R.string.applets_error_firmware,
                    Toast.LENGTH_SHORT
                ).show()
            }
        }
        root.findViewById<View>(R.id.console_dock_multiplayer).setOnClickListener {
            (fragment.activity as? MainActivity)?.displayMultiplayerDialog()
        }
        root.findViewById<View>(R.id.console_dock_about).setOnClickListener {
            navigateToSubscreen(SettingsSubscreen.ABOUT)
        }
    }

    private fun navigateToSubscreen(destination: SettingsSubscreen) {
        val action = HomeNavigationDirections.actionGlobalSettingsSubscreenActivity(destination, null)
        fragment.findNavController().navigate(action)
    }

    /**
     * Enables the console presentation only while the carousel view type is in use. With grid/list
     * views selected from the "view" menu the details panel and backdrop are hidden and the screen
     * behaves exactly as before (the dock stays available).
     */
    fun setActive(isCarousel: Boolean) {
        if (released) return
        active = isCarousel
        carousel.consoleStyle = isCarousel
        if (!isCarousel) {
            clearSelection()
            // Let the grid/list use the whole height above the dock.
            details.visibility = View.GONE
        } else if (details.visibility == View.GONE) {
            details.visibility = View.INVISIBLE
        }
    }

    /** Called by GamesFragment after every list submission (filter, search, reload). */
    fun onListSubmitted(isEmpty: Boolean) {
        if (released || !active) return
        if (isEmpty) {
            clearSelection()
        } else {
            // Positions may now point to different games; re-evaluate on the next frame.
            selectedGame = null
            // Note: not refreshView(), which would also steal focus from the search field.
            carousel.post { if (!released) carousel.updateChildScalesAndAlpha() }
        }
    }

    fun onResume() {
        if (released) return
        updateHintsVisibility()
    }

    fun release() {
        released = true
        backdropJob?.cancel()
        backdropJob = null
        details.removeCallbacks(applySelectionRunnable)
        details.animate().cancel()
        backdropA.animate().cancel()
        backdropB.animate().cancel()
        carousel.onCenteredPositionChanged = null
        carousel.keyInterceptor = null
        carousel.removeOnLayoutChangeListener(carouselLayoutListener)
        details.removeOnLayoutChangeListener(carouselLayoutListener)
    }

    // ---------------------------------------------------------------------------------------------
    // Selection & details

    private fun onCenteredPosition(position: Int) {
        if (!active || released) return
        pendingPosition = position
        // Coalesce: the carousel reports on every scroll frame / layout pass; touching text views
        // inside a layout pass would trigger another layout, so apply on the next frame.
        details.removeCallbacks(applySelectionRunnable)
        details.post(applySelectionRunnable)
    }

    private fun applySelection() {
        if (!active || released) return
        val game = adapter.currentList.getOrNull(pendingPosition) ?: return
        if (game == selectedGame) return
        selectedGame = game

        gameTitle.text = game.title.replace("[\\t\\n\\r]+".toRegex(), " ")
        gameMeta.text = buildMeta(game)

        if (details.visibility != View.VISIBLE) {
            details.visibility = View.VISIBLE
        }
        alignDetailsWithCard()
        details.animate().cancel()
        details.alpha = 0.25f
        details.translationY = detailsShiftPx
        details.animate()
            .alpha(1f)
            .translationY(0f)
            .setDuration(detailsFadeMs)
            .start()

        scheduleBackdrop(game)
    }

    private fun clearSelection() {
        selectedGame = null
        pendingPosition = RecyclerView.NO_POSITION
        details.removeCallbacks(applySelectionRunnable)
        details.animate().cancel()
        details.visibility = View.INVISIBLE
        backdropJob?.cancel()
        backdropGame = null
        fadeOut(backdropA)
        fadeOut(backdropB)
    }

    private fun buildMeta(game: Game): String {
        val parts = mutableListOf<String>()
        if (game.developer.isNotBlank()) parts.add(game.developer.trim())

        val playTimeSeconds = try {
            if (game.programId.isNotEmpty()) NativeLibrary.playTimeManagerGetPlayTime(game.programId) else 0L
        } catch (e: Exception) {
            0L
        }
        if (playTimeSeconds > 0) {
            val hours = playTimeSeconds / 3600
            val minutes = (playTimeSeconds % 3600) / 60
            val readable = if (hours > 0) {
                "$hours${res.getString(R.string.hours_abbr)} $minutes${res.getString(R.string.minutes_abbr)}"
            } else {
                "$minutes${res.getString(R.string.minutes_abbr)}"
            }
            parts.add("${res.getString(R.string.playtime)} $readable")
        }

        if (game.version.isNotBlank()) parts.add("v${game.version.trim()}")

        val extension = Uri.decode(game.path).substringAfterLast('.', "").lowercase(Locale.ROOT)
        if (extension in Game.extensions) parts.add(extension.uppercase(Locale.ROOT))

        if (game.isHomebrew) parts.add(res.getString(R.string.home_console_homebrew))

        return parts.joinToString("  ·  ")
    }

    /** Left-aligns the details panel with the selected (centered) tile, like the console layout. */
    private fun alignDetailsWithCard() {
        if (!active || released) return
        val swipe = carousel.parent?.parent as? View ?: return
        val minMargin = minDetailsMarginPx.toInt()
        var target = swipe.left + carousel.left + carousel.paddingLeft
        // Keep the whole panel on screen on narrow devices.
        if (details.width > 0 && root.width > 0) {
            target = minOf(target, root.width - details.width - minMargin)
        }
        target = target.coerceAtLeast(minMargin)
        val lp = details.layoutParams as? ViewGroup.MarginLayoutParams ?: return
        if (lp.marginStart != target) {
            lp.marginStart = target
            // Post: this can be triggered from a layout callback.
            details.post { if (!released) details.layoutParams = lp }
        }
    }

    // ---------------------------------------------------------------------------------------------
    // Backdrop

    private fun scheduleBackdrop(game: Game) {
        if (game == backdropGame) return
        backdropJob?.cancel()

        // Instant path when already cached (no coroutine, no delay).
        GameBackdropCache.peek(game)?.let {
            backdropGame = game
            crossfadeBackdrop(it)
            return
        }

        backdropJob = fragment.viewLifecycleOwner.lifecycleScope.launch {
            // Small delay so fast scrolling through many games doesn't load every icon.
            delay(backdropDelayMs)
            val bitmap = GameBackdropCache.load(fragment.viewLifecycleOwner, game)
            if (released || !active || selectedGame != game) return@launch
            backdropGame = game
            if (bitmap != null) {
                crossfadeBackdrop(bitmap)
            } else {
                // No artwork: fall back to the neutral background.
                fadeOut(backdropA)
                fadeOut(backdropB)
            }
        }
    }

    private fun crossfadeBackdrop(bitmap: Bitmap) {
        val incoming = if (frontIsA) backdropB else backdropA
        val outgoing = if (frontIsA) backdropA else backdropB
        frontIsA = !frontIsA

        incoming.animate().cancel()
        incoming.setImageBitmap(bitmap)
        incoming.animate()
            .alpha(backdropAlpha)
            .setDuration(backdropFadeMs)
            .start()
        fadeOut(outgoing)
    }

    private fun fadeOut(view: ImageView) {
        view.animate().cancel()
        if (view.alpha == 0f) {
            view.setImageDrawable(null)
            return
        }
        view.animate()
            .alpha(0f)
            .setDuration(backdropFadeMs)
            .withEndAction { if (view.alpha == 0f) view.setImageDrawable(null) }
            .start()
    }

    // ---------------------------------------------------------------------------------------------
    // Actions (reuse the existing GameAdapter click handlers so launch checks stay identical)

    private fun centeredItemView(): View? {
        val position = carousel.getClosestChildPosition()
        if (position == RecyclerView.NO_POSITION) return null
        return carousel.findViewHolderForAdapterPosition(position)?.itemView
    }

    private fun launchSelected() {
        centeredItemView()?.performClick()
    }

    private fun openOptionsForSelected() {
        centeredItemView()?.performLongClick()
    }

    private fun openInfoForSelected() {
        val game = selectedGame ?: return
        fragment.findNavController().navigate(R.id.gameInfoFragment, bundleOf("game" to game))
    }

    private fun handleCarouselKey(event: KeyEvent): Boolean {
        if (!active) return false
        return when (event.keyCode) {
            KeyEvent.KEYCODE_BUTTON_X -> {
                if (event.action == KeyEvent.ACTION_DOWN && event.repeatCount == 0) {
                    openOptionsForSelected()
                }
                true
            }
            KeyEvent.KEYCODE_BUTTON_Y -> {
                if (event.action == KeyEvent.ACTION_DOWN && event.repeatCount == 0) {
                    root.findViewById<View?>(R.id.search_background)?.performClick()
                }
                true
            }
            else -> false
        }
    }

    private fun updateHintsVisibility() {
        hints.visibility = if (isGamepadConnected()) View.VISIBLE else View.GONE
    }

    private fun isGamepadConnected(): Boolean {
        return InputDevice.getDeviceIds().any { id ->
            val sources = InputDevice.getDevice(id)?.sources ?: 0
            sources and InputDevice.SOURCE_GAMEPAD == InputDevice.SOURCE_GAMEPAD ||
                sources and InputDevice.SOURCE_JOYSTICK == InputDevice.SOURCE_JOYSTICK
        }
    }
}
