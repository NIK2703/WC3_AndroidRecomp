package wc3.recomp.launch

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.Environment
import androidx.core.content.ContextCompat
import wc3.recomp.R
import java.io.File

/**
 * Validates the launch settings and mirrors the native pick_data_dir()
 * contract.  User-facing text always comes from resources; this object only
 * chooses which resource to return.
 */
object LaunchValidator {
    const val MAX_PATH_BYTES = 1000
    const val MAX_ARGS_CHARS = 200
    const val MAX_LATENCY_MS = 500
    const val MIN_WIDTH = 320
    const val MAX_WIDTH = 7680
    const val MIN_HEIGHT = 240
    const val MAX_HEIGHT = 4320

    sealed class Result {
        data object Ok : Result()
        data class Error(val kind: Kind, val message: String) : Result()
    }

    enum class Kind {
        PERMISSION,
        FOLDER,
        FILES,
        WRITABLE,
        RESOLUTION,
        ARGS,
        LATENCY
    }

    fun validate(context: Context, settings: LaunchSettings): Result {
        val rawDir = settings.gameDir.trim()
        val folder = resolveGameDir(context, settings)

        if (rawDir.isNotEmpty()) {
            if (!folder.isAbsolute) {
                return error(context, Kind.FOLDER, R.string.launch_error_folder, rawDir)
            }
            if (rawDir.toByteArray(Charsets.UTF_8).size > MAX_PATH_BYTES) {
                return error(context, Kind.FOLDER, R.string.launch_error_folder, rawDir)
            }
        }

        if (!hasStoragePermission(context, folder)) {
            return Result.Error(
                Kind.PERMISSION,
                context.getString(R.string.launch_error_permission)
            )
        }
        if (!folder.exists()) {
            return error(context, Kind.FOLDER, R.string.launch_error_folder,
                folder.absolutePath)
        }
        if (!folder.isDirectory) {
            return error(context, Kind.FOLDER, R.string.launch_error_not_directory,
                folder.absolutePath)
        }
        if (!folder.canRead() || !folder.canExecute()) {
            return error(context, Kind.FOLDER, R.string.launch_error_folder,
                folder.absolutePath)
        }
        if (!folder.canWrite()) {
            return error(context, Kind.WRITABLE, R.string.launch_error_not_writable,
                folder.absolutePath)
        }

        val missing = requiredFiles(settings.mode)
            .filter { findFile(folder, it) == null }
        if (missing.isNotEmpty()) {
            return Result.Error(
                Kind.FILES,
                context.getString(
                    R.string.launch_error_missing_files,
                    missing.joinToString(", ")
                )
            )
        }

        val width = settings.width
        val height = settings.height
        val resolutionOk = (width == 0 && height == 0) ||
            (width in MIN_WIDTH..MAX_WIDTH && height in MIN_HEIGHT..MAX_HEIGHT)
        if (!resolutionOk) {
            return Result.Error(
                Kind.RESOLUTION,
                context.getString(R.string.launch_error_resolution)
            )
        }

        val args = settings.extraArgs
        if (args.length > MAX_ARGS_CHARS ||
            args.any { it.code < 0x20 || it.code > 0x7E }
        ) {
            return Result.Error(
                Kind.ARGS,
                context.getString(R.string.launch_error_args)
            )
        }

        if (settings.audioLatencyMs !in 0..MAX_LATENCY_MS) {
            return Result.Error(
                Kind.LATENCY,
                context.getString(R.string.launch_error_latency)
            )
        }

        return Result.Ok
    }

    fun resolveGameDir(context: Context, settings: LaunchSettings): File {
        val raw = settings.gameDir.trim()
        if (raw.isNotEmpty()) return File(raw)
        return autoDetectGameDir(context)
            ?: File(Environment.getExternalStorageDirectory(), DEFAULT_GAME_DIR_NAME)
    }

    fun hasStoragePermission(context: Context, folder: File): Boolean {
        if (isAppPrivate(context, folder)) return true

        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            Environment.isExternalStorageManager()
        } else {
            val read = ContextCompat.checkSelfPermission(
                context,
                Manifest.permission.READ_EXTERNAL_STORAGE
            ) == PackageManager.PERMISSION_GRANTED
            val write = ContextCompat.checkSelfPermission(
                context,
                Manifest.permission.WRITE_EXTERNAL_STORAGE
            ) == PackageManager.PERMISSION_GRANTED
            read && write
        }
    }

    fun findGameDir(context: Context): File? =
        candidateDirs(context).firstOrNull { dir ->
            hasStoragePermission(context, dir) && hasCommonFiles(dir)
        }

    /** Mirrors the native pick_data_dir() order for an empty game directory. */
    private fun autoDetectGameDir(context: Context): File? =
        nativeCandidateDirs(context).firstOrNull { dir ->
            hasStoragePermission(context, dir) && hasCommonFiles(dir)
        }

    private fun nativeCandidateDirs(context: Context): List<File> = listOfNotNull(
        File(Environment.getExternalStorageDirectory(), DEFAULT_GAME_DIR_NAME),
        context.getExternalFilesDir(null),
        context.filesDir
    )

    fun requiredFiles(mode: GameMode): List<String> = when (mode) {
        GameMode.TFT -> COMMON_FILES + TFT_FILES
        GameMode.ROC -> COMMON_FILES
    }

    private fun error(
        context: Context,
        kind: Kind,
        messageRes: Int,
        detail: String
    ): Result.Error = Result.Error(kind, context.getString(messageRes, detail))

    private fun candidateDirs(context: Context): List<File> {
        val external = Environment.getExternalStorageDirectory()
        val dirs = mutableListOf(
            File(external, DEFAULT_GAME_DIR_NAME),
            File(external, "Warcraft III"),
            File(external, "Games/Warcraft III"),
            File(external, "Download/Warcraft III"),
            context.filesDir
        )
        context.getExternalFilesDir(null)?.let {
            dirs += it
            dirs += File(it, DEFAULT_GAME_DIR_NAME)
        }
        return dirs
    }

    private fun hasCommonFiles(dir: File): Boolean =
        dir.isDirectory &&
            dir.canRead() &&
            dir.canExecute() &&
            dir.canWrite() &&
            COMMON_FILES.all { findFile(dir, it) != null }

    private fun findFile(dir: File, name: String): File? =
        File(dir, name).takeIf { it.isFile }

    private fun isAppPrivate(context: Context, folder: File): Boolean {
        val path = folder.absolutePath
        val roots = listOfNotNull(
            context.dataDir,
            context.filesDir,
            context.cacheDir,
            context.getExternalFilesDir(null),
            context.getExternalCacheDir()
        )
        return roots.any { root ->
            path == root.absolutePath ||
                path.startsWith(root.absolutePath + File.separator)
        }
    }

    private const val DEFAULT_GAME_DIR_NAME = "Warcraft 3"
    private val COMMON_FILES = listOf("war3.exe", "game.dll", "war3.mpq")
    // Casing matches the native asset checks in host_shell.c.  Shared storage
    // resolves either case; app-private ext4 must match exactly.
    private val TFT_FILES = listOf("War3x.mpq", "War3xlocal.mpq", "War3Patch.mpq")
}
