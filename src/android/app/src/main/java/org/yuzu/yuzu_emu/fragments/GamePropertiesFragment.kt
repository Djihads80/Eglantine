// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.fragments

import android.content.Intent
import android.content.pm.ShortcutInfo
import android.content.pm.ShortcutManager
import android.net.Uri
import android.os.Bundle
import android.provider.DocumentsContract
import android.view.InputDevice
import android.view.KeyEvent
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.documentfile.provider.DocumentFile
import androidx.fragment.app.Fragment
import androidx.fragment.app.activityViewModels
import androidx.lifecycle.lifecycleScope
import androidx.navigation.findNavController
import androidx.navigation.fragment.navArgs
import androidx.recyclerview.widget.GridLayoutManager
import com.google.android.material.transition.MaterialSharedAxis
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.yuzu.yuzu_emu.HomeNavigationDirections
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.YuzuApplication
import org.yuzu.yuzu_emu.adapters.GamePropertiesAdapter
import org.yuzu.yuzu_emu.databinding.FragmentGamePropertiesBinding
import org.yuzu.yuzu_emu.features.DocumentProvider
import org.yuzu.yuzu_emu.features.settings.model.BooleanSetting
import org.yuzu.yuzu_emu.features.settings.model.Settings
import org.yuzu.yuzu_emu.features.settings.ui.SettingsSubscreen
import org.yuzu.yuzu_emu.features.settings.utils.SettingsFile
import org.yuzu.yuzu_emu.model.AddonViewModel
import org.yuzu.yuzu_emu.model.DriverViewModel
import org.yuzu.yuzu_emu.model.Game
import org.yuzu.yuzu_emu.model.GameProperty
import org.yuzu.yuzu_emu.model.GamesViewModel
import org.yuzu.yuzu_emu.model.HomeViewModel
import org.yuzu.yuzu_emu.model.InstallableProperty
import org.yuzu.yuzu_emu.model.SubMenuPropertySecondaryAction
import org.yuzu.yuzu_emu.model.SubmenuProperty
import org.yuzu.yuzu_emu.model.TaskState
import org.yuzu.yuzu_emu.utils.DirectoryInitialization
import org.yuzu.yuzu_emu.utils.FileUtil
import org.yuzu.yuzu_emu.utils.GameHelper
import org.yuzu.yuzu_emu.utils.GameIconUtils
import org.yuzu.yuzu_emu.utils.GpuDriverHelper
import org.yuzu.yuzu_emu.utils.MemoryUtil
import org.yuzu.yuzu_emu.utils.NativeConfig
import org.yuzu.yuzu_emu.utils.ViewUtils.marquee
import org.yuzu.yuzu_emu.utils.ViewUtils.setVisible
import org.yuzu.yuzu_emu.utils.collect
import java.io.BufferedOutputStream
import java.io.File
import java.util.Locale

class GamePropertiesFragment : Fragment() {
    private var _binding: FragmentGamePropertiesBinding? = null
    private val binding get() = _binding!!

    private val homeViewModel: HomeViewModel by activityViewModels()
    private val gamesViewModel: GamesViewModel by activityViewModels()
    private val addonViewModel: AddonViewModel by activityViewModels()
    private val driverViewModel: DriverViewModel by activityViewModels()

    private val args by navArgs<GamePropertiesFragmentArgs>()

    // Console game screen: options grouped in tabs (game settings / content / data).
    private enum class Tab { SETTINGS, CONTENT, DATA }

    private var currentTab = Tab.SETTINGS

    // Values computed off the main thread and shown as chips on the option cards.
    private val addonsDetails = MutableStateFlow("")
    private val saveSizeDetails = MutableStateFlow("")

    companion object {
        private const val KEY_TAB = "ConsoleGameTab"
        private const val KEY_POST_SHADER_ENABLED = "post_shader_enabled"
        private const val KEY_POST_SHADER_PRESET = "post_shader_preset"
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enterTransition = MaterialSharedAxis(MaterialSharedAxis.Y, true)
        returnTransition = MaterialSharedAxis(MaterialSharedAxis.Y, false)
        reenterTransition = MaterialSharedAxis(MaterialSharedAxis.X, false)
    }

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View {
        _binding = FragmentGamePropertiesBinding.inflate(layoutInflater)
        return binding.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        homeViewModel.setStatusBarShadeVisibility(true)

        binding.buttonBack.setOnClickListener {
            view.findNavController().popBackStack()
        }

        val shortcutManager = requireActivity().getSystemService(ShortcutManager::class.java)
        binding.buttonShortcut.isEnabled = shortcutManager.isRequestPinShortcutSupported
        binding.buttonShortcut.setOnClickListener {
            viewLifecycleOwner.lifecycleScope.launch {
                withContext(Dispatchers.IO) {
                    val shortcut = ShortcutInfo.Builder(requireContext(), args.game.title)
                        .setShortLabel(args.game.title)
                        .setIcon(
                            GameIconUtils.getShortcutIcon(requireActivity(), args.game)
                                .toIcon(requireContext())
                        )
                        .setIntent(args.game.launchIntent)
                        .build()
                    shortcutManager.requestPinShortcut(shortcut, null)
                }
            }
        }

        GameIconUtils.loadGameIcon(args.game, binding.imageGameScreen)
        binding.title.text = args.game.title
        binding.title.marquee()
        bindGameHeader()

        getPlayTime()

        binding.buttonStart.setOnClickListener {
            LaunchGameDialogFragment.newInstance(args.game)
                .show(childFragmentManager, LaunchGameDialogFragment.TAG)
        }

        if (GameHelper.cachedGameList.isEmpty()) {
            binding.buttonStart.isEnabled = false
            viewLifecycleOwner.lifecycleScope.launch {
                withContext(Dispatchers.IO) {
                    GameHelper.restoreContentForGame(args.game)
                }
                if (_binding == null) {
                    return@launch
                }
                addonViewModel.onAddonsViewStarted(args.game)
                binding.buttonStart.isEnabled = true
            }
        }

        if (savedInstanceState != null) {
            currentTab = Tab.entries.getOrElse(savedInstanceState.getInt(KEY_TAB)) { Tab.SETTINGS }
        }
        setupTabs()
        reloadList()
        setupGamepad()

        // Add-ons count for the "Add-ons" card (reuses AddonViewModel's patch list).
        if (GameHelper.cachedGameList.isNotEmpty()) {
            addonViewModel.onAddonsViewStarted(args.game)
        }
        addonViewModel.addonList.collect(viewLifecycleOwner) { patches ->
            if (addonViewModel.game?.path != args.game.path || patches.isEmpty()) {
                addonsDetails.value = ""
            } else {
                val enabled = patches.count { it.enabled }
                addonsDetails.value = resources.getQuantityString(
                    R.plurals.game_console_addons_active,
                    enabled,
                    enabled
                )
            }
        }

        homeViewModel.openImportSaves.collect(
            viewLifecycleOwner,
            resetState = { homeViewModel.setOpenImportSaves(false) }
        ) { if (it) importSaves.launch(arrayOf("application/zip")) }
        homeViewModel.reloadPropertiesList.collect(
            viewLifecycleOwner,
            resetState = { homeViewModel.reloadPropertiesList(false) }
        ) { if (it) reloadList() }

        setInsets()
    }

    override fun onDestroy() {
        val isChangingConfigurations = activity?.isChangingConfigurations == true
        super.onDestroy()
        if (!isChangingConfigurations) {
            gamesViewModel.reloadGames(true)
        }
    }

    private fun getPlayTime() {
        binding.playtime.text = buildString {
            val playTimeSeconds = NativeLibrary.playTimeManagerGetPlayTime(args.game.programId)

            val hours = playTimeSeconds / 3600
            val minutes = (playTimeSeconds % 3600) / 60
            val seconds = playTimeSeconds % 60

            val readablePlayTime = when {
            hours > 0 -> "$hours${getString(R.string.hours_abbr)} $minutes${getString(R.string.minutes_abbr)} $seconds${getString(R.string.seconds_abbr)}"
            minutes > 0 -> "$minutes${getString(R.string.minutes_abbr)} $seconds${getString(R.string.seconds_abbr)}"
            else -> "$seconds${getString(R.string.seconds_abbr)}"
}

            append(getString(R.string.playtime) + " " + readablePlayTime)
        }

        binding.playtime.setOnClickListener {
            showEditPlaytimeDialog()
        }
    }

    private fun showEditPlaytimeDialog() {
        val dialogView = layoutInflater.inflate(R.layout.dialog_edit_playtime, null)
        val hoursLayout =
            dialogView.findViewById<com.google.android.material.textfield.TextInputLayout>(R.id.layout_hours)
        val minutesLayout =
            dialogView.findViewById<com.google.android.material.textfield.TextInputLayout>(R.id.layout_minutes)
        val secondsLayout =
            dialogView.findViewById<com.google.android.material.textfield.TextInputLayout>(R.id.layout_seconds)
        val hoursInput =
            dialogView.findViewById<com.google.android.material.textfield.TextInputEditText>(R.id.input_hours)
        val minutesInput =
            dialogView.findViewById<com.google.android.material.textfield.TextInputEditText>(R.id.input_minutes)
        val secondsInput =
            dialogView.findViewById<com.google.android.material.textfield.TextInputEditText>(R.id.input_seconds)

        val playTimeSeconds = NativeLibrary.playTimeManagerGetPlayTime(args.game.programId)
        val hours = playTimeSeconds / 3600
        val minutes = (playTimeSeconds % 3600) / 60
        val seconds = playTimeSeconds % 60

        hoursInput.setText(hours.toString())
        minutesInput.setText(minutes.toString())
        secondsInput.setText(seconds.toString())

        val dialog = com.google.android.material.dialog.MaterialAlertDialogBuilder(requireContext())
            .setTitle(R.string.edit_playtime)
            .setView(dialogView)
            .setPositiveButton(android.R.string.ok, null)
            .setNegativeButton(android.R.string.cancel, null)
            .create()

        dialog.setOnShowListener {
            val positiveButton = dialog.getButton(android.app.AlertDialog.BUTTON_POSITIVE)
            positiveButton.setOnClickListener {
                hoursLayout.error = null
                minutesLayout.error = null
                secondsLayout.error = null

                val hoursText = hoursInput.text.toString()
                val minutesText = minutesInput.text.toString()
                val secondsText = secondsInput.text.toString()

                val hoursValue = hoursText.toLongOrNull() ?: 0
                val minutesValue = minutesText.toLongOrNull() ?: 0
                val secondsValue = secondsText.toLongOrNull() ?: 0

                var hasError = false

                // normally cant be above 9999
                if (hoursValue < 0 || hoursValue > 9999) {
                    hoursLayout.error = getString(R.string.hours_must_be_between_0_and_9999)
                    hasError = true
                }

                if (minutesValue < 0 || minutesValue > 59) {
                    minutesLayout.error = getString(R.string.minutes_must_be_between_0_and_59)
                    hasError = true
                }

                if (secondsValue < 0 || secondsValue > 59) {
                    secondsLayout.error = getString(R.string.seconds_must_be_between_0_and_59)
                    hasError = true
                }

                if (!hasError) {
                    val totalSeconds = hoursValue * 3600 + minutesValue * 60 + secondsValue
                    NativeLibrary.playTimeManagerSetPlayTime(args.game.programId, totalSeconds)
                    getPlayTime()
                    Toast.makeText(
                        requireContext(),
                        R.string.playtime_updated_successfully,
                        Toast.LENGTH_SHORT
                    ).show()
                    dialog.dismiss()
                }
            }
        }

        dialog.show()
    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        outState.putInt(KEY_TAB, currentTab.ordinal)
    }

    /** Breadcrumb, version, developer and file info (format / program ID / size). */
    private fun bindGameHeader() {
        val game = args.game
        binding.consoleBreadcrumb.text =
            "${getString(R.string.game_console_library)}  /  ${game.title}"

        val version = game.version.trim()
        binding.consoleGameVersion.text = if (version.isNotEmpty()) "v$version" else ""
        binding.consoleGameVersion.setVisible(version.isNotEmpty())

        val developer = game.developer.trim()
        binding.consoleGameDeveloper.text = getString(R.string.game_console_by, developer)
        binding.consoleGameDeveloper.setVisible(developer.isNotEmpty())

        val extension = Uri.decode(game.path).substringAfterLast('.', "").lowercase(Locale.ROOT)
        val parts = mutableListOf<String>()
        if (game.programIdHex != "0") parts.add(game.programIdHex)
        if (extension in Game.extensions) parts.add(extension.uppercase(Locale.ROOT))
        binding.consoleGameFileInfo.text = parts.joinToString(" · ")
        binding.consoleGameFileInfo.setVisible(parts.isNotEmpty())

        // File size through the existing NativeLibrary.getSize() (document provider / native path).
        viewLifecycleOwner.lifecycleScope.launch {
            val bytes = withContext(Dispatchers.IO) {
                try {
                    NativeLibrary.getSize(game.path)
                } catch (e: Exception) {
                    0L
                }
            }
            if (_binding == null || bytes <= 0L) return@launch
            parts.add(MemoryUtil.bytesToSizeUnit(bytes.toFloat()))
            binding.consoleGameFileInfo.text = parts.joinToString(" · ")
            binding.consoleGameFileInfo.setVisible(true)
        }
    }

    private fun tabViews() = mapOf(
        Tab.SETTINGS to binding.tabSettings,
        Tab.CONTENT to binding.tabContent,
        Tab.DATA to binding.tabData
    )

    private fun setupTabs() {
        tabViews().forEach { (tab, view) ->
            view.setOnClickListener { selectTab(tab) }
        }
        updateTabSelection()
    }

    private fun selectTab(tab: Tab) {
        if (tab == currentTab) return
        currentTab = tab
        updateTabSelection()
        reloadList()
    }

    private fun updateTabSelection() {
        tabViews().forEach { (tab, view) -> view.isSelected = tab == currentTab }
    }

    /**
     * Gamepad: X launches the game, L1/R1 switch tabs. Hints are shown only with a controller
     * connected; confirm/back glyphs follow the "invert confirm/back" app setting.
     */
    private fun setupGamepad() {
        ViewCompat.addOnUnhandledKeyEventListener(binding.root) { _, event ->
            if (event.action != KeyEvent.ACTION_DOWN || event.repeatCount != 0) {
                return@addOnUnhandledKeyEventListener false
            }
            when (event.keyCode) {
                KeyEvent.KEYCODE_BUTTON_X -> {
                    if (binding.buttonStart.isEnabled) binding.buttonStart.performClick()
                    true
                }
                KeyEvent.KEYCODE_BUTTON_L1 -> {
                    selectTab(Tab.entries[(currentTab.ordinal + Tab.entries.size - 1) % Tab.entries.size])
                    true
                }
                KeyEvent.KEYCODE_BUTTON_R1 -> {
                    selectTab(Tab.entries[(currentTab.ordinal + 1) % Tab.entries.size])
                    true
                }
                else -> false
            }
        }
        updateInputHints()
    }

    private fun updateInputHints() {
        val gamepad = InputDevice.getDeviceIds().any { id ->
            val sources = InputDevice.getDevice(id)?.sources ?: 0
            sources and InputDevice.SOURCE_GAMEPAD == InputDevice.SOURCE_GAMEPAD ||
                sources and InputDevice.SOURCE_JOYSTICK == InputDevice.SOURCE_JOYSTICK
        }
        val inverted = BooleanSetting.INVERT_CONFIRM_BACK_CONTROLLER_BUTTONS.getBoolean()
        binding.consoleHintConfirmGlyph.text = if (inverted) "B" else "A"
        binding.consoleHintBackGlyph.text = if (inverted) "A" else "B"
        binding.consoleHints.setVisible(gamepad)
    }

    private class PerGameSummary(
        val frameGen: Boolean,
        val postProcessing: Boolean,
        val postPreset: String
    )

    /**
     * Reads the values shown on the frame generation / post-processing cards from this game's
     * configuration (same load/unload pattern as DriverViewModel.updateDriverNameForGame).
     */
    private fun readPerGameSummary(): PerGameSummary? {
        val loadedHere = !NativeConfig.isPerGameConfigLoaded()
        return try {
            if (loadedHere) SettingsFile.loadCustomConfig(args.game)
            PerGameSummary(
                frameGen = NativeConfig.getBoolean(BooleanSetting.RENDERER_FRAME_GEN.key, false),
                postProcessing = NativeConfig.getBoolean(KEY_POST_SHADER_ENABLED, false),
                postPreset = NativeConfig.getString(KEY_POST_SHADER_PRESET, false).trim()
            )
        } catch (e: Exception) {
            null
        } finally {
            if (loadedHere) {
                NativeConfig.unloadPerGameConfig()
                NativeConfig.reloadGlobalConfig()
            }
        }
    }

    private fun updateSaveSize() {
        val saveDir = File(args.game.saveDir)
        viewLifecycleOwner.lifecycleScope.launch {
            val bytes = withContext(Dispatchers.IO) {
                if (saveDir.exists()) {
                    saveDir.walkTopDown().filter { it.isFile }.sumOf { it.length() }
                } else {
                    -1L
                }
            }
            saveSizeDetails.value =
                if (bytes >= 0L) MemoryUtil.bytesToSizeUnit(bytes.toFloat()) else ""
        }
    }

    private fun formatPlayTime(seconds: Long): String {
        val hours = seconds / 3600
        val minutes = (seconds % 3600) / 60
        return if (hours > 0) {
            "$hours${getString(R.string.hours_abbr)} $minutes${getString(R.string.minutes_abbr)}"
        } else {
            "$minutes${getString(R.string.minutes_abbr)}"
        }
    }

    private fun reloadList() {
        _binding ?: return
        driverViewModel.updateDriverNameForGame(args.game)

        val properties = when (currentTab) {
            Tab.SETTINGS -> settingsTabProperties()
            Tab.CONTENT -> contentTabProperties()
            Tab.DATA -> dataTabProperties()
        }

        binding.listProperties.apply {
            layoutManager = GridLayoutManager(
                context,
                resources.getInteger(R.integer.console_game_property_columns)
            )
            adapter = GamePropertiesAdapter(viewLifecycleOwner, properties)
        }
    }

    /** Game settings: per-game configuration, GPU driver, post-processing, frame generation, Freedreno. */
    private fun settingsTabProperties(): List<GameProperty> {
        val configFile = File(
            DirectoryInitialization.userDirectory +
                "/config/custom/" + args.game.settingsName + ".ini"
        )
        val configExists = configFile.exists()
        val summary = readPerGameSummary()

        return mutableListOf<GameProperty>().apply {
            add(
                SubmenuProperty(
                    R.string.game_console_custom_config,
                    R.string.per_game_settings_description,
                    R.drawable.ic_settings,
                    details = {
                        if (configExists) getString(R.string.game_console_custom_config_active) else ""
                    },
                    action = {
                        val action = HomeNavigationDirections.actionGlobalSettingsActivity(
                            args.game,
                            Settings.MenuTag.SECTION_ROOT
                        )
                        binding.root.findNavController().navigate(action)
                    },
                    secondaryActions = listOf(
                        SubMenuPropertySecondaryAction(
                            isShown = true,
                            descriptionId = R.string.import_config,
                            iconId = R.drawable.ic_import,
                            action = { importConfig.launch(arrayOf("*/*")) }
                        ),
                        SubMenuPropertySecondaryAction(
                            isShown = configExists,
                            descriptionId = R.string.export_config,
                            iconId = R.drawable.ic_export,
                            action = { exportConfig.launch(args.game.settingsName + ".ini") }
                        ),
                        SubMenuPropertySecondaryAction(
                            isShown = configExists,
                            descriptionId = R.string.share_game_settings,
                            iconId = R.drawable.ic_share,
                            action = {
                                if (configFile.exists()) {
                                    shareConfigFile(configFile)
                                }
                            }
                        )
                    )
                )
            )
            if (GpuDriverHelper.supportsCustomDriverLoading()) {
                add(
                    SubmenuProperty(
                        R.string.gpu_driver_manager,
                        R.string.install_gpu_driver_description,
                        R.drawable.ic_build,
                        detailsFlow = driverViewModel.selectedDriverTitle,
                        action = {
                            val action =
                                HomeNavigationDirections.actionGlobalSettingsSubscreenActivity(
                                    SettingsSubscreen.DRIVER_MANAGER,
                                    args.game
                                )
                            binding.root.findNavController().navigate(action)
                        }
                    )
                )
            }
            add(
                SubmenuProperty(
                    R.string.post_processing,
                    R.string.post_processing_per_game_description,
                    R.drawable.ic_post_processing,
                    details = {
                        when {
                            summary == null -> ""
                            !summary.postProcessing -> getString(R.string.game_console_off)
                            summary.postPreset.isNotEmpty() -> summary.postPreset
                            else -> getString(R.string.game_console_on)
                        }
                    },
                    action = {
                        val action = HomeNavigationDirections.actionGlobalSettingsActivity(
                            args.game,
                            Settings.MenuTag.SECTION_POST_PROCESSING
                        )
                        binding.root.findNavController().navigate(action)
                    }
                )
            )
            add(
                SubmenuProperty(
                    R.string.frame_gen,
                    R.string.frame_gen_per_game_description,
                    R.drawable.ic_duck,
                    details = {
                        when {
                            summary == null -> ""
                            summary.frameGen -> getString(R.string.game_console_on)
                            else -> getString(R.string.game_console_off)
                        }
                    },
                    action = {
                        val action = HomeNavigationDirections.actionGlobalSettingsActivity(
                            args.game,
                            Settings.MenuTag.SECTION_FRAME_GEN
                        )
                        binding.root.findNavController().navigate(action)
                    }
                )
            )
            if (GpuDriverHelper.isAdrenoGpu()) {
                add(
                    SubmenuProperty(
                        R.string.freedreno_per_game_title,
                        R.string.freedreno_per_game_description,
                        R.drawable.ic_graphics,
                        action = {
                            val action =
                                HomeNavigationDirections.actionGlobalSettingsSubscreenActivity(
                                    SettingsSubscreen.FREEDRENO_SETTINGS,
                                    args.game
                                )
                            binding.root.findNavController().navigate(action)
                        }
                    )
                )
            }
        }
    }

    /** Content: game information (and integrity check) and add-ons. */
    private fun contentTabProperties(): List<GameProperty> =
        mutableListOf<GameProperty>().apply {
            add(
                SubmenuProperty(
                    R.string.info,
                    R.string.info_description,
                    R.drawable.ic_info_outline,
                    details = { args.game.version.trim() },
                    action = {
                        val action = HomeNavigationDirections.actionGlobalSettingsSubscreenActivity(
                            SettingsSubscreen.GAME_INFO,
                            args.game
                        )
                        binding.root.findNavController().navigate(action)
                    }
                )
            )
            if (!args.game.isHomebrew) {
                add(
                    SubmenuProperty(
                        R.string.add_ons,
                        R.string.add_ons_description,
                        R.drawable.ic_edit,
                        detailsFlow = addonsDetails,
                        action = {
                            val action =
                                HomeNavigationDirections.actionGlobalSettingsSubscreenActivity(
                                    SettingsSubscreen.ADDONS,
                                    args.game
                                )
                            binding.root.findNavController().navigate(action)
                        }
                    )
                )
            }
        }

    /** Data: save data, shader cache and playtime. */
    private fun dataTabProperties(): List<GameProperty> =
        mutableListOf<GameProperty>().apply {
            if (!args.game.isHomebrew) {
                updateSaveSize()
                add(
                    InstallableProperty(
                        R.string.save_data,
                        R.string.save_data_description,
                        R.drawable.ic_save,
                        {
                            MessageDialogFragment.newInstance(
                                requireActivity(),
                                titleId = R.string.import_save_warning,
                                descriptionId = R.string.import_save_warning_description,
                                positiveAction = { homeViewModel.setOpenImportSaves(true) }
                            ).show(parentFragmentManager, MessageDialogFragment.TAG)
                        },
                        if (File(args.game.saveDir).exists()) {
                            { exportSaves.launch(args.game.saveZipName) }
                        } else {
                            null
                        },
                        detailsFlow = saveSizeDetails
                    )
                )
                val saveDirFile = File(args.game.saveDir)
                if (saveDirFile.exists()) {
                    add(
                        SubmenuProperty(
                            R.string.delete_save_data,
                            R.string.delete_save_data_description,
                            R.drawable.ic_delete,
                            isDestructive = true,
                            action = {
                                MessageDialogFragment.newInstance(
                                    requireActivity(),
                                    titleId = R.string.delete_save_data,
                                    descriptionId = R.string.delete_save_data_warning_description,
                                    positiveButtonTitleId = android.R.string.cancel,
                                    negativeButtonTitleId = android.R.string.ok,
                                    negativeAction = {
                                        File(args.game.saveDir).deleteRecursively()
                                        Toast.makeText(
                                            YuzuApplication.appContext,
                                            R.string.save_data_deleted_successfully,
                                            Toast.LENGTH_SHORT
                                        ).show()
                                        homeViewModel.reloadPropertiesList(true)
                                    }
                                ).show(parentFragmentManager, MessageDialogFragment.TAG)
                            }
                        )
                    )
                }

                val shaderCacheDir = File(
                    DirectoryInitialization.userDirectory +
                        "/cache/shader/" + args.game.settingsName.lowercase()
                )
                if (shaderCacheDir.exists()) {
                    add(
                        SubmenuProperty(
                            R.string.clear_shader_cache,
                            R.string.clear_shader_cache_description,
                            R.drawable.ic_delete,
                            isDestructive = true,
                            details = {
                                if (shaderCacheDir.exists()) {
                                    val bytes = shaderCacheDir.walkTopDown().filter { it.isFile }
                                        .map { it.length() }.sum()
                                    MemoryUtil.bytesToSizeUnit(bytes.toFloat())
                                } else {
                                    MemoryUtil.bytesToSizeUnit(0f)
                                }
                            },
                            action = {
                                MessageDialogFragment.newInstance(
                                    requireActivity(),
                                    titleId = R.string.clear_shader_cache,
                                    descriptionId = R.string.clear_shader_cache_warning_description,
                                    positiveAction = {
                                        shaderCacheDir.deleteRecursively()
                                        Toast.makeText(
                                            YuzuApplication.appContext,
                                            R.string.cleared_shaders_successfully,
                                            Toast.LENGTH_SHORT
                                        ).show()
                                        homeViewModel.reloadPropertiesList(true)
                                    }
                                ).show(parentFragmentManager, MessageDialogFragment.TAG)
                            }
                        )
                    )
                }

                val playTimeSeconds = NativeLibrary.playTimeManagerGetPlayTime(args.game.programId)
                add(
                    SubmenuProperty(
                        R.string.game_console_playtime,
                        R.string.game_console_playtime_description,
                        R.drawable.ic_clock,
                        details = { formatPlayTime(playTimeSeconds) },
                        action = { showEditPlaytimeDialog() },
                        secondaryActions = listOf(
                            SubMenuPropertySecondaryAction(
                                isShown = playTimeSeconds > 0,
                                descriptionId = R.string.reset_playtime,
                                iconId = R.drawable.ic_delete,
                                action = {
                                    MessageDialogFragment.newInstance(
                                        requireActivity(),
                                        titleId = R.string.reset_playtime,
                                        descriptionId = R.string.reset_playtime_warning_description,
                                        positiveAction = {
                                            NativeLibrary.playTimeManagerResetProgramPlayTime(
                                                args.game.programId
                                            )
                                            Toast.makeText(
                                                YuzuApplication.appContext,
                                                R.string.playtime_reset_successfully,
                                                Toast.LENGTH_SHORT
                                            ).show()
                                            getPlayTime()
                                            homeViewModel.reloadPropertiesList(true)
                                        }
                                    ).show(parentFragmentManager, MessageDialogFragment.TAG)
                                }
                            )
                        )
                    )
                )
            }
        }

    override fun onResume() {
        super.onResume()
        driverViewModel.updateDriverNameForGame(args.game)
        getPlayTime()
        reloadList()
        updateInputHints()
    }

    private fun setInsets() =
        ViewCompat.setOnApplyWindowInsetsListener(
            binding.root
        ) { view: View, windowInsets: WindowInsetsCompat ->
            val barInsets = windowInsets.getInsets(WindowInsetsCompat.Type.systemBars())
            val cutoutInsets = windowInsets.getInsets(WindowInsetsCompat.Type.displayCutout())
            view.setPadding(
                barInsets.left + cutoutInsets.left,
                maxOf(barInsets.top, cutoutInsets.top),
                barInsets.right + cutoutInsets.right,
                barInsets.bottom
            )
            windowInsets
        }

    private val importSaves =
        registerForActivityResult(ActivityResultContracts.OpenDocument()) { result ->
            if (result == null) {
                return@registerForActivityResult
            }

            val savesFolder = File(args.game.saveDir)
            val cacheSaveDir = File("${requireContext().cacheDir.path}/saves/")
            cacheSaveDir.mkdir()

            ProgressDialogFragment.newInstance(
                requireActivity(),
                R.string.save_files_importing,
                false
            ) { _, _ ->
                try {
                    FileUtil.unzipToInternalStorage(result.toString(), cacheSaveDir)
                    val files = cacheSaveDir.listFiles()
                    var savesFolderFile: File? = null
                    if (files != null) {
                        val savesFolderName = args.game.programIdHex
                        for (file in files) {
                            if (file.isDirectory && file.name == savesFolderName) {
                                savesFolderFile = file
                                break
                            }
                        }
                    }

                    if (savesFolderFile != null) {
                        savesFolder.deleteRecursively()
                        savesFolder.mkdir()
                        savesFolderFile.copyRecursively(savesFolder)
                        savesFolderFile.deleteRecursively()
                    }

                    withContext(Dispatchers.Main) {
                        if (savesFolderFile == null) {
                            MessageDialogFragment.newInstance(
                                requireActivity(),
                                titleId = R.string.save_file_invalid_zip_structure,
                                descriptionId = R.string.save_file_invalid_zip_structure_description
                            ).show(parentFragmentManager, MessageDialogFragment.TAG)
                            return@withContext
                        }
                        Toast.makeText(
                            YuzuApplication.appContext,
                            getString(R.string.save_file_imported_success),
                            Toast.LENGTH_LONG
                        ).show()
                        homeViewModel.reloadPropertiesList(true)
                    }

                    cacheSaveDir.deleteRecursively()
                } catch (e: Exception) {
                    Toast.makeText(
                        YuzuApplication.appContext,
                        getString(R.string.fatal_error),
                        Toast.LENGTH_LONG
                    ).show()
                }
            }.show(parentFragmentManager, ProgressDialogFragment.TAG)
        }

    /**
     * Exports the save file located in the given folder path by creating a zip file and opening a
     * file picker to save.
     */
    private val exportSaves = registerForActivityResult(
        ActivityResultContracts.CreateDocument("application/zip")
    ) { result ->
        if (result == null) {
            return@registerForActivityResult
        }

        ProgressDialogFragment.newInstance(
            requireActivity(),
            R.string.save_files_exporting,
            false
        ) { _, _ ->
            val saveLocation = args.game.saveDir
            val zipResult = FileUtil.zipFromInternalStorage(
                File(saveLocation),
                saveLocation.replaceAfterLast("/", ""),
                BufferedOutputStream(requireContext().contentResolver.openOutputStream(result)),
                compression = false
            )
            return@newInstance when (zipResult) {
                TaskState.Completed -> getString(R.string.export_success)
                TaskState.Cancelled, TaskState.Failed -> getString(R.string.export_failed)
            }
        }.show(parentFragmentManager, ProgressDialogFragment.TAG)
    }

    /**
     * Imports an ini file from external storage to internal app directory and override per-game config
     */
    private val importConfig = registerForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { result ->
        if (result == null) {
            return@registerForActivityResult
        }

        val iniResult = FileUtil.copyUriToInternalStorage(
            sourceUri = result,
            destinationParentPath =
                DirectoryInitialization.userDirectory + "/config/custom/",
            destinationFilename = args.game.settingsName + ".ini"
        )
        if (iniResult?.exists() == true) {
            Toast.makeText(
                requireContext(),
                getString(R.string.import_success),
                Toast.LENGTH_SHORT
            ).show()
            homeViewModel.reloadPropertiesList(true)
        } else {
            Toast.makeText(
                requireContext(),
                getString(R.string.import_failed),
                Toast.LENGTH_SHORT
            ).show()
        }
    }

    /**
     * Exports game's config ini to the specified location in external storage
     */
    private val exportConfig = registerForActivityResult(
        ActivityResultContracts.CreateDocument("text/ini")
    ) { result ->
        if (result == null) {
            return@registerForActivityResult
        }

        ProgressDialogFragment.newInstance(
            requireActivity(),
            R.string.save_files_exporting,
            false
        ) { _, _ ->
            val configLocation = DirectoryInitialization.userDirectory +
                "/config/custom/" + args.game.settingsName + ".ini"

            val iniResult = FileUtil.copyToExternalStorage(
                sourcePath = configLocation,
                destUri = result
            )
            return@newInstance when (iniResult) {
                TaskState.Completed -> getString(R.string.export_success)
                TaskState.Cancelled, TaskState.Failed -> getString(R.string.export_failed)
            }
        }.show(parentFragmentManager, ProgressDialogFragment.TAG)
    }

    private fun shareConfigFile(configFile: File) {
        val file = DocumentFile.fromSingleUri(
            requireContext(),
            DocumentsContract.buildDocumentUri(
                DocumentProvider.AUTHORITY,
                "${DocumentProvider.ROOT_ID}/${configFile}"
            )
        )!!

        val intent = Intent(Intent.ACTION_SEND)
            .setDataAndType(file.uri, FileUtil.TEXT_PLAIN)
            .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        if (file.exists()) {
            intent.putExtra(Intent.EXTRA_STREAM, file.uri)
            startActivity(Intent.createChooser(intent, getText(R.string.share_game_settings)))
        } else {
            Toast.makeText(
                requireContext(),
                getText(R.string.share_config_failed),
                Toast.LENGTH_SHORT
            ).show()
        }
    }
}
