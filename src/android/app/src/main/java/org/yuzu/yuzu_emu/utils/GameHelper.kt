// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.yuzu.yuzu_emu.utils

import android.content.SharedPreferences
import android.net.Uri
import android.provider.DocumentsContract
import androidx.preference.PreferenceManager
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import java.io.File
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.YuzuApplication
import org.yuzu.yuzu_emu.model.Game
import org.yuzu.yuzu_emu.model.GameDir
import org.yuzu.yuzu_emu.model.MinimalDocumentFile
import androidx.core.content.edit
import androidx.core.net.toUri

object GameHelper {
    private const val KEY_OLD_GAME_PATH = "game_path"
    const val KEY_GAMES = "Games"

    var cachedGameList = mutableListOf<Game>()

    private lateinit var preferences: SharedPreferences

    fun getGames(): List<Game> {
        val games = mutableListOf<Game>()
        val gamesByProgramId = mutableMapOf<String, Game>()
        val context = YuzuApplication.appContext
        preferences = PreferenceManager.getDefaultSharedPreferences(context)

        val gameDirs = mutableListOf<GameDir>()
        val oldGamesDir = preferences.getString(KEY_OLD_GAME_PATH, "") ?: ""
        if (oldGamesDir.isNotEmpty()) {
            gameDirs.add(GameDir(oldGamesDir, true))
            preferences.edit() { remove(KEY_OLD_GAME_PATH) }
        }
        gameDirs.addAll(NativeConfig.getGameDirs())

        // Ensure keys are loaded so that ROM metadata can be decrypted.
        NativeLibrary.reloadKeys()

        // Reset metadata so we don't use stale information
        GameMetadata.resetMetadata()

        // Remove previous filesystem provider information so we can get up to date version info
        NativeLibrary.clearFilesystemProvider()

        val mountedContainerUris = mutableSetOf<String>()
        mountExternalContentDirectories(mountedContainerUris)

        val badDirs = mutableListOf<Int>()
        gameDirs.forEachIndexed { index: Int, gameDir: GameDir ->
            val gameDirUri = gameDir.uriString.toUri()
            val isValid = FileUtil.isTreeUriValid(gameDirUri)
            if (isValid) {
                val scanDepth = if (gameDir.deepScan) 3 else 1

                val gameDirFiles = FileUtil.listFiles(gameDirUri)
                val looseMain = findLooseExeFsMain(gameDirFiles)
                if (looseMain != null) {
                    addLooseExeFsGame(games, gamesByProgramId, looseMain)
                } else {
                    addGamesRecursive(
                        games,
                        gamesByProgramId,
                        gameDirFiles,
                        scanDepth,
                        mountedContainerUris
                    )
                }
            } else {
                badDirs.add(index)
            }
        }

        // Remove all game dirs with insufficient permissions from config
        if (badDirs.isNotEmpty()) {
            var offset = 0
            badDirs.forEach {
                gameDirs.removeAt(it - offset)
                offset++
            }
        }
        NativeConfig.setGameDirs(gameDirs.toTypedArray())

        // Cache list of games found on disk
        val serializedGames = mutableSetOf<String>()
        games.forEach {
            serializedGames.add(Json.encodeToString(it))
        }
        preferences.edit() {
            remove(KEY_GAMES)
                .putStringSet(KEY_GAMES, serializedGames)
        }

        cachedGameList = games.toMutableList()
        return games.toList()
    }

    fun restoreContentForGame(game: Game) {
        NativeLibrary.reloadKeys()

        val mountedContainerUris = mutableSetOf<String>()
        mountExternalContentDirectories(mountedContainerUris)
        mountGameFolderContent(Uri.parse(game.path), mountedContainerUris)
        NativeLibrary.addFileToFilesystemProvider(game.path)
    }

    // File extensions considered as external content, buuut should
    // be done better imo.
    private val externalContentExtensions = setOf("nsp", "xci", "nsz", "xcz", "nup")

    private fun scanContentContainersRecursive(
        files: Array<MinimalDocumentFile>,
        depth: Int,
        onContainerFound: (MinimalDocumentFile) -> Unit
    ) {
        if (depth <= 0) {
            return
        }

        files.forEach {
            if (it.isDirectory) {
                scanContentContainersRecursive(
                    FileUtil.listFiles(it.uri),
                    depth - 1,
                    onContainerFound
                )
            } else {
                val extension = FileUtil.getExtension(it.uri).lowercase()
                if (externalContentExtensions.contains(extension)) {
                    onContainerFound(it)
                }
            }
        }
    }

    private fun addGamesRecursive(
        games: MutableList<Game>,
        gamesByProgramId: MutableMap<String, Game>,
        files: Array<MinimalDocumentFile>,
        depth: Int,
        mountedContainerUris: MutableSet<String>
    ) {
        if (depth <= 0) {
            return
        }

        files.forEach {
            if (it.isDirectory) {
                val children = FileUtil.listFiles(it.uri)
                // A loose ExeFS dump (how Switch 2 titles boot) is a game, not a folder to scan.
                // Checked at every depth, shallow scans too: main is one or two levels down.
                val looseMain = findLooseExeFsMain(children)
                if (looseMain != null) {
                    addLooseExeFsGame(games, gamesByProgramId, looseMain)
                } else {
                    addGamesRecursive(
                        games,
                        gamesByProgramId,
                        children,
                        depth - 1,
                        mountedContainerUris
                    )
                }
            } else {
                val extension = FileUtil.getExtension(it.uri).lowercase()
                val filePath = it.uri.toString()

                val mountedContainer = externalContentExtensions.contains(extension) &&
                    mountedContainerUris.add(filePath)
                if (mountedContainer) {
                    NativeLibrary.addGameFolderFileToFilesystemProvider(filePath)
                }

                if (Game.extensions.contains(extension)) {
                    val game = getGame(it.uri, true, false)
                    if (game != null) {
                        games.add(game)
                        if (game.programId != "0") {
                            gamesByProgramId[game.programId] = game
                        }
                    } else if (mountedContainer) {
                        GameMetadata.getProgramId(filePath).toLongOrNull()?.let { programId ->
                            gamesByProgramId[(programId and 0x800L.inv()).toString()]
                        }?.let { existingGame ->
                            NativeLibrary.getPatchesForFile(existingGame.path, existingGame.programId)
                            existingGame.version = GameMetadata.getVersion(
                                existingGame.path,
                                true
                            )
                            GameIconUtils.refreshGameIcon(existingGame)
                        }
                    }
                }
            }
        }
    }

    /**
     * main of a loose ExeFS dump: main and main.npdm side by side in [files], or in their
     * exefs/ subdirectory. Null for anything else.
     */
    fun findLooseExeFsMain(files: Array<MinimalDocumentFile>): Uri? {
        fun mainOf(entries: Array<MinimalDocumentFile>): Uri? {
            val main = entries.firstOrNull { !it.isDirectory && it.filename == "main" }
                ?: return null
            val hasNpdm = entries.any { !it.isDirectory && it.filename == "main.npdm" }
            return if (hasNpdm) main.uri else null
        }

        mainOf(files)?.let { return it }
        val exefs = files.firstOrNull {
            it.isDirectory && it.filename.equals("exefs", ignoreCase = true)
        } ?: return null
        return mainOf(FileUtil.listFiles(exefs.uri))
    }

    private fun addLooseExeFsGame(
        games: MutableList<Game>,
        gamesByProgramId: MutableMap<String, Game>,
        mainUri: Uri
    ) {
        val game = getGame(mainUri, true, false) ?: return
        games.add(game)
        if (game.programId != "0") {
            gamesByProgramId[game.programId] = game
        }
    }

    // Name of a loose ExeFS dump's folder: the one holding exefs/, else the one holding main.
    private fun looseExeFsDumpName(mainPath: String): String? {
        val dir = FileUtil.getParentUri(mainPath) ?: return null
        val dirName = FileUtil.getFilename(dir.toUri())
        if (!dirName.equals("exefs", ignoreCase = true)) {
            return dirName
        }
        val titleDir = FileUtil.getParentUri(dir) ?: return dirName
        return FileUtil.getFilename(titleDir.toUri())
    }

    private fun mountExternalContentDirectories(mountedContainerUris: MutableSet<String>) {
        val uniqueExternalContentDirs = linkedSetOf<String>()
        NativeConfig.getExternalContentDirs().forEach { externalDir ->
            if (externalDir.isNotEmpty()) {
                uniqueExternalContentDirs.add(externalDir)
            }
        }

        for (externalDir in uniqueExternalContentDirs) {
            val externalDirUri = externalDir.toUri()
            if (FileUtil.isTreeUriValid(externalDirUri)) {
                scanContentContainersRecursive(FileUtil.listFiles(externalDirUri), 3) {
                    val containerUri = it.uri.toString()
                    if (mountedContainerUris.add(containerUri)) {
                        NativeLibrary.addFileToFilesystemProvider(containerUri)
                    }
                }
            }
        }
    }

    private fun mountGameFolderContent(gameUri: Uri, mountedContainerUris: MutableSet<String>) {
        if (gameUri.scheme == "content") {
            val parentUri = getParentDocumentUri(gameUri) ?: return
            scanContentContainersRecursive(FileUtil.listFiles(parentUri), 1) {
                val containerUri = it.uri.toString()
                if (mountedContainerUris.add(containerUri)) {
                    NativeLibrary.addGameFolderFileToFilesystemProvider(containerUri)
                }
            }
            return
        }

        val gameFile = File(gameUri.path ?: gameUri.toString())
        val parentDir = gameFile.parentFile ?: return
        parentDir.listFiles()?.forEach { sibling ->
            if (!sibling.isFile) {
                return@forEach
            }

            val extension = sibling.extension.lowercase()
            if (externalContentExtensions.contains(extension)) {
                val containerUri = Uri.fromFile(sibling).toString()
                if (mountedContainerUris.add(containerUri)) {
                    NativeLibrary.addGameFolderFileToFilesystemProvider(containerUri)
                }
            }
        }
    }

    private fun getParentDocumentUri(uri: Uri): Uri? {
        return try {
            val documentId = DocumentsContract.getDocumentId(uri)
            val separatorIndex = documentId.lastIndexOf('/')
            if (separatorIndex == -1) {
                null
            } else {
                val parentDocumentId = documentId.substring(0, separatorIndex)
                DocumentsContract.buildDocumentUriUsingTree(uri, parentDocumentId)
            }
        } catch (_: Exception) {
            null
        }
    }

    fun getGame(
        uri: Uri,
        addedToLibrary: Boolean,
        registerFilesystemProvider: Boolean = true
    ): Game? {
        val filePath = uri.toString()
        if (!GameMetadata.getIsValid(filePath)) {
            return null
        }

        if (registerFilesystemProvider) {
            // Needed to update installed content information
            NativeLibrary.addFileToFilesystemProvider(filePath)
        }

        var name = GameMetadata.getTitle(filePath)

        // If the game's title field is empty, use the filename (for a loose ExeFS, its folder's).
        if (name.isEmpty()) {
            name = FileUtil.getFilename(uri)
            if (name == "main") {
                name = looseExeFsDumpName(filePath) ?: name
            }
        }
        var programId = GameMetadata.getProgramId(filePath)

        // If the game's ID field is empty, use the filename without extension.
        if (programId.isEmpty()) {
            programId = name.substringBeforeLast(".")
        }

        val newGame = Game(
            name,
            filePath,
            programId,
            GameMetadata.getDeveloper(filePath),
            GameMetadata.getVersion(filePath, false),
            GameMetadata.getIsHomebrew(filePath)
        )

        if (addedToLibrary) {
            val addedTime = preferences.getLong(newGame.keyAddedToLibraryTime, 0L)
            if (addedTime == 0L) {
                preferences.edit()
                    .putLong(newGame.keyAddedToLibraryTime, System.currentTimeMillis())
                    .apply()
            }
        }

        return newGame
    }
}
