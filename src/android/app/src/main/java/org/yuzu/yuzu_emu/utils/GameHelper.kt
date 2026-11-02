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
import org.yuzu.yuzu_emu.getPublicFilesDir
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

                addGamesRecursive(
                    games,
                    gamesByProgramId,
                    FileUtil.listFiles(gameDirUri),
                    scanDepth,
                    mountedContainerUris
                )
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

        addLocalExtractedGames(games, gamesByProgramId, gameDirs)

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
    private val externalContentExtensions = setOf("nsp", "xci", "nsz", "xcz")

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
                addGamesRecursive(
                    games,
                    gamesByProgramId,
                    FileUtil.listFiles(it.uri),
                    depth - 1,
                    mountedContainerUris
                )
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

    /**
     * Extracted games (an exefs folder holding "main", with romfs beside it) placed under
     * <app files>/games/<name>/ are opened by plain path, since the deconstructed ROM loader
     * needs to see sibling folders, which content URIs do not allow.
     */
    /** Maps an external-storage tree URI (e.g. primary:Games) to a plain path. */
    private fun treeUriToPath(uriString: String): File? {
        return try {
            val uri = uriString.toUri()
            if (uri.authority != "com.android.externalstorage.documents") return null
            val docId = DocumentsContract.getTreeDocumentId(uri)
            val volume = docId.substringBefore(':')
            val relative = docId.substringAfter(':', "")
            val base = if (volume.equals("primary", true)) {
                android.os.Environment.getExternalStorageDirectory()
            } else {
                File("/storage/$volume")
            }
            if (relative.isEmpty()) base else File(base, relative)
        } catch (e: Exception) {
            null
        }
    }

    private var askedForAllFilesAccess = false

    private fun requestAllFilesAccess() {
        if (askedForAllFilesAccess) return
        askedForAllFilesAccess = true
        android.os.Handler(android.os.Looper.getMainLooper()).post {
            val context = YuzuApplication.appContext
            android.widget.Toast.makeText(
                context,
                "Allow \"All files access\" for Eden NS2 so it can open extracted Switch 2 games in your game folders",
                android.widget.Toast.LENGTH_LONG
            ).show()
            try {
                val intent = android.content.Intent(
                    android.provider.Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    "package:${context.packageName}".toUri()
                ).addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK)
                context.startActivity(intent)
            } catch (e: Exception) {
                Log.warning("[GameHelper] Could not open all-files-access settings: ${e.message}")
            }
        }
    }

    private fun addLocalExtractedGames(
        games: MutableList<Game>,
        gamesByProgramId: MutableMap<String, Game>,
        gameDirs: List<GameDir>
    ) {
        val root = File(YuzuApplication.appContext.getPublicFilesDir(), "games")
        if (!root.isDirectory) {
            root.mkdirs()
        }

        // Any "main" with a "main.npdm" beside it, up to four folders deep.
        val mains = mutableListOf<File>()
        fun search(dir: File, depth: Int) {
            if (depth < 0) return
            val main = File(dir, "main")
            if (main.isFile && File(dir, "main.npdm").isFile) {
                mains.add(main)
                return
            }
            dir.listFiles()?.filter { it.isDirectory }?.forEach { search(it, depth - 1) }
        }
        search(root, 4)

        // Game folders added in Eden are content URIs, which the folder loader can't
        // use; with all-files access, scan them again by plain path.
        val folderPaths = gameDirs.mapNotNull { treeUriToPath(it.uriString) }.distinct()
        if (folderPaths.isNotEmpty()) {
            if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.R &&
                !android.os.Environment.isExternalStorageManager()
            ) {
                requestAllFilesAccess()
            } else {
                folderPaths.forEach { if (it.isDirectory) search(it, 4) }
            }
        }

        val report = mutableListOf<String>()
        mains.sortedBy { it.absolutePath }.forEach { main ->
            val path = main.absolutePath
            val shortPath = main.parentFile?.parentFile?.name ?: main.path
            if (!GameMetadata.getIsValid(path)) {
                report.add("Not recognized: $shortPath")
                Log.warning("[GameHelper] Extracted game not recognized: $path")
                return@forEach
            }
            // Walk up past exefs/, base/ and update folders (e.g. updates/917504/exefs) to the
            // folder named after the game.
            var gameDir = main.parentFile!!
            var isUpdate = false
            while (gameDir.parentFile != null) {
                val name = gameDir.name.lowercase()
                if (name in setOf("updates", "update")) {
                    isUpdate = true
                } else if (name !in setOf("exefs", "base") && !name.all { it.isDigit() }) {
                    break
                }
                gameDir = gameDir.parentFile!!
            }
            val folderTitle = if (isUpdate) "${gameDir.name} (update)" else gameDir.name
            val title = GameMetadata.getTitle(path).ifEmpty { folderTitle }
            val programId = GameMetadata.getProgramId(path).ifEmpty { gameDir.name }
            val game = Game(
                title,
                path,
                programId,
                GameMetadata.getDeveloper(path),
                GameMetadata.getVersion(path, false),
                GameMetadata.getIsHomebrew(path)
            )
            games.add(game)
            if (programId != "0") {
                gamesByProgramId[programId] = game
            }
            report.add("Added: $title")
        }

        val folders = root.listFiles()?.count { it.isDirectory } ?: 0
        if (mains.isEmpty() && folders == 0) {
            return
        }
        val message = if (mains.isEmpty()) {
            "Extracted games: $folders folder(s) in games/, but no exefs/main with main.npdm found"
        } else {
            report.joinToString("\n")
        }
        Log.info("[GameHelper] $message")
        run {
            android.os.Handler(android.os.Looper.getMainLooper()).post {
                android.widget.Toast.makeText(
                    YuzuApplication.appContext,
                    message,
                    android.widget.Toast.LENGTH_LONG
                ).show()
            }
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

        // If the game's title field is empty, use the filename.
        if (name.isEmpty()) {
            name = FileUtil.getFilename(uri)
        }
        var programId = GameMetadata.getProgramId(filePath)

        // If the game's ID field is empty, use the filename without extension.
        if (programId.isEmpty()) {
            programId = name.substring(0, name.lastIndexOf("."))
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
