// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.ui.home

import android.graphics.Color
import android.graphics.drawable.ColorDrawable
import android.net.Uri
import android.view.Gravity
import android.view.InputDevice
import android.view.KeyEvent
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.EditText
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.PopupWindow
import android.widget.TextView
import android.widget.Toast
import androidx.fragment.app.Fragment
import androidx.lifecycle.lifecycleScope
import androidx.navigation.fragment.findNavController
import androidx.recyclerview.widget.RecyclerView
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.yuzu.yuzu_emu.HomeNavigationDirections
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.adapters.GameAdapter
import org.yuzu.yuzu_emu.features.settings.model.BooleanSetting
import org.yuzu.yuzu_emu.features.settings.ui.SettingsSubscreen
import org.yuzu.yuzu_emu.model.Game
import org.yuzu.yuzu_emu.ui.CarouselRecyclerView
import org.yuzu.yuzu_emu.ui.main.MainActivity
import org.yuzu.yuzu_emu.utils.MemoryUtil
import java.util.Locale

/**
 * Presentation layer of the console-style Home (portrait and landscape).
 *
 * It only *decorates* the existing games screen: GamesFragment, GameAdapter, GamesViewModel and the
 * launch flow are reused untouched. This class:
 *  - switches the carousel into its flat console style,
 *  - follows the centered game and shows its details (title, version, developer, playtime,
 *    format and file size),
 *  - wires the Play button to the existing card click handler (same launch checks),
 *  - provides the gear menu (settings, search, view, sort, install, multiplayer, applets, about),
 *  - adds gamepad shortcuts (X = game options, Y = search).
 *
 * Nothing here runs continuously: work happens only on selection changes.
 */
class ConsoleHomeController private constructor(
    private val fragment: Fragment,
    private val root: View,
    private val carousel: CarouselRecyclerView,
    private val adapter: GameAdapter,
    private val actions: Actions
) {
    /** Existing GamesFragment behaviour reused by the gear menu. */
    class Actions(
        val openSettings: () -> Unit,
        val focusSearch: () -> Unit,
        val showViewMenu: (anchor: View) -> Unit,
        val showSortMenu: (anchor: View) -> Unit
    )

    private val details: View = root.findViewById(R.id.console_details)
    private val gameTitle: TextView = root.findViewById(R.id.console_game_title)
    private val gameVersion: TextView = root.findViewById(R.id.console_game_version)
    private val metaDeveloper: TextView = root.findViewById(R.id.console_meta_developer)
    private val metaPlaytime: TextView = root.findViewById(R.id.console_meta_playtime)
    private val metaFormat: TextView = root.findViewById(R.id.console_meta_format)
    private val metaSize: TextView = root.findViewById(R.id.console_meta_size)
    private val playButton: View = root.findViewById(R.id.console_play_button)
    private val playGlyph: TextView = root.findViewById(R.id.console_play_glyph)
    private val playIcon: View = root.findViewById(R.id.console_play_icon)
    private val hints: View = root.findViewById(R.id.console_hints)
    private val hintConfirmGlyph: TextView = root.findViewById(R.id.console_hint_confirm_glyph)
    private val menuButton: View = root.findViewById(R.id.settings_button)
    private val headerSpace: View = root.findViewById(R.id.console_header_space)
    private val searchFrame: View = root.findViewById(R.id.frame_search)
    private val searchText: EditText = root.findViewById(R.id.search_text)

    private val res = root.resources
    private val detailsFadeMs = res.getInteger(R.integer.console_home_details_fade_ms).toLong()
    private val detailsShiftPx = res.getDimension(R.dimen.console_home_details_shift)

    private var active = false
    private var released = false
    private var selectedGame: Game? = null
    private var pendingPosition = RecyclerView.NO_POSITION
    private var sizeJob: Job? = null
    private var menu: PopupWindow? = null

    private val applySelectionRunnable = Runnable { applySelection() }

    companion object {
        // File sizes by game path, kept for the process lifetime (querying the provider is slow).
        private val sizeCache = HashMap<String, Long>()

        /**
         * Attaches the console Home if the current layout provides it (res/layout/fragment_games.xml).
         * Returns null for layouts without console views, leaving behaviour unchanged.
         */
        fun attachIfSupported(
            fragment: Fragment,
            root: View,
            carousel: CarouselRecyclerView?,
            adapter: GameAdapter,
            actions: Actions
        ): ConsoleHomeController? {
            if (carousel == null || root.findViewById<View?>(R.id.console_details) == null) {
                return null
            }
            return ConsoleHomeController(fragment, root, carousel, adapter, actions).also { it.bind() }
        }
    }

    private fun bind() {
        carousel.onCenteredPositionChanged = { position, _ -> onCenteredPosition(position) }
        carousel.keyInterceptor = ::handleCarouselKey

        playButton.setOnClickListener { launchSelected() }
        // UP from Play returns to the selected tile (not whichever card is geometrically above).
        playButton.setOnKeyListener { _, keyCode, event ->
            if (keyCode == KeyEvent.KEYCODE_DPAD_UP && event.action == KeyEvent.ACTION_DOWN) {
                carousel.focusCenteredCard()
                true
            } else {
                false
            }
        }

        root.findViewById<View?>(R.id.console_profile_button)?.setOnClickListener {
            navigateToSubscreen(SettingsSubscreen.PROFILE_MANAGER)
        }

        // The search field only takes room in the top bar while it is in use.
        searchText.setOnFocusChangeListener { _, hasFocus ->
            if (!hasFocus) hideSearchIfEmpty()
        }
        if (searchText.text.isNotEmpty()) showSearch(focus = false)

        updateInputHints()
    }

    /**
     * Enables the console presentation only while the carousel view type is in use. With grid/list
     * views selected from the "view" menu the details panel and Play button are hidden and the
     * screen behaves as before.
     */
    fun setActive(isCarousel: Boolean) {
        if (released) return
        active = isCarousel
        carousel.consoleStyle = isCarousel
        if (!isCarousel) {
            clearSelection()
            details.visibility = View.GONE
            playButton.visibility = View.GONE
            hints.visibility = View.GONE
        } else {
            if (details.visibility == View.GONE) details.visibility = View.INVISIBLE
            if (playButton.visibility == View.GONE) playButton.visibility = View.INVISIBLE
            updateInputHints()
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
        updateInputHints()
        // Playtime changes after playing: refresh the details of the selected game.
        if (active) {
            selectedGame = null
            carousel.post { if (!released) carousel.updateChildScalesAndAlpha() }
        }
    }

    fun release() {
        released = true
        sizeJob?.cancel()
        sizeJob = null
        menu?.dismiss()
        menu = null
        details.removeCallbacks(applySelectionRunnable)
        details.animate().cancel()
        carousel.onCenteredPositionChanged = null
        carousel.keyInterceptor = null
        searchText.onFocusChangeListener = null
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
        val version = game.version.trim()
        gameVersion.text = if (version.isNotEmpty()) "v$version" else ""
        gameVersion.visibility = if (version.isNotEmpty()) View.VISIBLE else View.GONE

        val developer = game.developer.trim()
        metaDeveloper.text = res.getString(R.string.home_console_meta_developer, developer)
        metaDeveloper.visibility = if (developer.isNotEmpty()) View.VISIBLE else View.GONE

        metaPlaytime.text = res.getString(
            R.string.home_console_meta_playtime,
            formatPlayTime(readPlayTime(game))
        )
        metaFormat.text = res.getString(R.string.home_console_meta_format, formatOf(game))
        bindSize(game)

        details.visibility = View.VISIBLE
        playButton.visibility = View.VISIBLE
        details.animate().cancel()
        details.alpha = 0.25f
        details.translationY = detailsShiftPx
        details.animate()
            .alpha(1f)
            .translationY(0f)
            .setDuration(detailsFadeMs)
            .start()
    }

    private fun clearSelection() {
        selectedGame = null
        pendingPosition = RecyclerView.NO_POSITION
        sizeJob?.cancel()
        details.removeCallbacks(applySelectionRunnable)
        details.animate().cancel()
        details.visibility = View.INVISIBLE
        playButton.visibility = View.INVISIBLE
    }

    private fun readPlayTime(game: Game): Long = try {
        if (game.programId.isNotEmpty()) NativeLibrary.playTimeManagerGetPlayTime(game.programId) else 0L
    } catch (e: Exception) {
        0L
    }

    private fun formatPlayTime(seconds: Long): String {
        val hours = seconds / 3600
        val minutes = (seconds % 3600) / 60
        val h = res.getString(R.string.hours_abbr)
        val m = res.getString(R.string.minutes_abbr)
        return if (hours > 0) "$hours$h $minutes$m" else "$minutes$m"
    }

    private fun formatOf(game: Game): String {
        val extension = Uri.decode(game.path).substringAfterLast('.', "").lowercase(Locale.ROOT)
        val format = if (extension in Game.extensions) extension.uppercase(Locale.ROOT) else "-"
        return if (game.isHomebrew) "$format · ${res.getString(R.string.home_console_homebrew)}" else format
    }

    /** File size from the existing NativeLibrary.getSize() (document provider / native path). */
    private fun bindSize(game: Game) {
        sizeJob?.cancel()
        val cached = sizeCache[game.path]
        if (cached != null) {
            showSize(cached)
            return
        }
        metaSize.visibility = View.INVISIBLE
        sizeJob = fragment.viewLifecycleOwner.lifecycleScope.launch {
            val bytes = withContext(Dispatchers.IO) {
                try {
                    NativeLibrary.getSize(game.path)
                } catch (e: Exception) {
                    0L
                }
            }
            sizeCache[game.path] = bytes
            if (!released && selectedGame == game) showSize(bytes)
        }
    }

    private fun showSize(bytes: Long) {
        if (bytes <= 0L) {
            metaSize.visibility = View.GONE
            return
        }
        metaSize.text = res.getString(
            R.string.home_console_meta_size,
            MemoryUtil.bytesToSizeUnit(bytes.toFloat())
        )
        metaSize.visibility = View.VISIBLE
    }

    // ---------------------------------------------------------------------------------------------
    // Gear menu

    private class MenuEntry(val iconId: Int, val labelId: Int, val action: (View) -> Unit)

    fun showMenu(anchor: View) {
        if (released) return
        menu?.dismiss()
        val context = anchor.context
        val inflater = LayoutInflater.from(context)
        val density = res.displayMetrics.density

        val entries = listOf(
            MenuEntry(R.drawable.ic_settings, R.string.preferences_settings) { actions.openSettings() },
            MenuEntry(R.drawable.ic_search, R.string.home_console_search) { showSearch(focus = true) },
            MenuEntry(R.drawable.ic_eye, R.string.home_console_view) { actions.showViewMenu(it) },
            MenuEntry(R.drawable.ic_filter, R.string.home_console_sort) { actions.showSortMenu(it) },
            MenuEntry(R.drawable.ic_install, R.string.install) {
                navigateToSubscreen(SettingsSubscreen.INSTALLABLE)
            },
            MenuEntry(R.drawable.ic_two_users, R.string.multiplayer) {
                (fragment.activity as? MainActivity)?.displayMultiplayerDialog()
            },
            MenuEntry(R.drawable.ic_applet, R.string.home_console_applets) { openApplets() },
            MenuEntry(R.drawable.ic_info_outline, R.string.about) {
                navigateToSubscreen(SettingsSubscreen.ABOUT)
            }
        )

        val content = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundResource(R.drawable.console_menu_background)
            val pad = (8 * density).toInt()
            setPadding(pad, pad, pad, pad)
            minimumWidth = (240 * density).toInt()
        }
        val popup = PopupWindow(
            content,
            ViewGroup.LayoutParams.WRAP_CONTENT,
            ViewGroup.LayoutParams.WRAP_CONTENT,
            true
        ).apply {
            setBackgroundDrawable(ColorDrawable(Color.TRANSPARENT))
            elevation = 12 * density
            setOnDismissListener { if (menu === this) menu = null }
        }

        var first: View? = null
        for (entry in entries) {
            val item = inflater.inflate(R.layout.item_console_menu, content, false)
            item.findViewById<ImageView>(R.id.console_menu_icon).setImageResource(entry.iconId)
            item.findViewById<TextView>(R.id.console_menu_label).setText(entry.labelId)
            item.setOnClickListener {
                popup.dismiss()
                entry.action(anchor)
            }
            content.addView(item)
            if (first == null) first = item
        }

        menu = popup
        popup.showAsDropDown(anchor, 0, (8 * density).toInt(), Gravity.START)
        // Gamepad / keyboard: start on the first entry. Touch mode is unaffected.
        first?.requestFocus()
    }

    private fun openApplets() {
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

    private fun navigateToSubscreen(destination: SettingsSubscreen) {
        val action = HomeNavigationDirections.actionGlobalSettingsSubscreenActivity(destination, null)
        fragment.findNavController().navigate(action)
    }

    // ---------------------------------------------------------------------------------------------
    // Search

    private fun showSearch(focus: Boolean) {
        headerSpace.visibility = View.GONE
        searchFrame.visibility = View.VISIBLE
        if (focus) actions.focusSearch()
    }

    private fun hideSearchIfEmpty() {
        if (released || searchText.text.isNotEmpty()) return
        searchFrame.visibility = View.GONE
        headerSpace.visibility = View.VISIBLE
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

    /** Long click on a game card opens its properties screen. */
    private fun openOptionsForSelected() {
        centeredItemView()?.performLongClick()
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
                    showSearch(focus = true)
                }
                true
            }
            else -> false
        }
    }

    /**
     * Shows the button glyphs only with a controller connected. The confirm glyph follows the
     * existing "invert confirm/back controller buttons" app setting.
     */
    private fun updateInputHints() {
        val gamepad = isGamepadConnected()
        val confirm = if (BooleanSetting.INVERT_CONFIRM_BACK_CONTROLLER_BUTTONS.getBoolean()) "B" else "A"
        playGlyph.text = confirm
        hintConfirmGlyph.text = confirm
        playGlyph.visibility = if (gamepad) View.VISIBLE else View.GONE
        playIcon.visibility = if (gamepad) View.GONE else View.VISIBLE
        hints.visibility = if (gamepad && active) View.VISIBLE else View.GONE
    }

    private fun isGamepadConnected(): Boolean {
        return InputDevice.getDeviceIds().any { id ->
            val sources = InputDevice.getDevice(id)?.sources ?: 0
            sources and InputDevice.SOURCE_GAMEPAD == InputDevice.SOURCE_GAMEPAD ||
                sources and InputDevice.SOURCE_JOYSTICK == InputDevice.SOURCE_JOYSTICK
        }
    }
}
