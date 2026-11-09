package wc3.recomp.launch

import android.content.Context
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.os.storage.StorageManager
import android.provider.DocumentsContract
import java.io.File

/**
 * Turns a tree URI returned by ACTION_OPEN_DOCUMENT_TREE into the real
 * filesystem path the native host needs.  The host reads the licensed game
 * files with POSIX calls, so a content:// URI is not usable directly.
 */
object GameFolderPicker {
    /**
     * Resolves a tree on the external-storage provider to an absolute path.
     * Returns null for providers that do not expose a filesystem path (cloud
     * drives, archive providers, and similar).
     */
    fun resolveTree(context: Context, uri: Uri): File? {
        if (!DocumentsContract.isTreeUri(uri)) return null

        val treeId = runCatching {
            DocumentsContract.getTreeDocumentId(uri)
        }.getOrNull() ?: return null

        // A few providers expose the real path directly as raw:<path>.
        if (treeId.startsWith(RAW_PREFIX)) {
            return absoluteFolder(treeId.substring(RAW_PREFIX.length))
        }

        if (uri.authority != EXTERNAL_STORAGE_AUTHORITY) return null

        val separator = treeId.indexOf(':')
        if (separator < 0) return null

        val volumeId = treeId.substring(0, separator)
        val relativePath = treeId.substring(separator + 1)
        if (volumeId.isEmpty() || volumeId.contains('/') || volumeId.contains('\\')) {
            return null
        }
        val root = volumeRoot(context, volumeId) ?: return null
        val folder = if (relativePath.isEmpty()) root else File(root, relativePath)
        return containedFolder(root, folder)
    }

    private fun absoluteFolder(path: String): File? {
        if (path.isEmpty()) return null
        val folder = File(path)
        return if (folder.isAbsolute) folder else null
    }

    private fun volumeRoot(context: Context, volumeId: String): File? {
        if (volumeId.equals(PRIMARY_VOLUME, ignoreCase = true)) {
            return Environment.getExternalStorageDirectory()
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            val manager = context.getSystemService(StorageManager::class.java)
            manager?.storageVolumes?.firstOrNull {
                it.uuid?.equals(volumeId, ignoreCase = true) == true
            }?.directory?.let { return it }
        }

        // StorageVolume.getDirectory() is API 30+.  On older releases the
        // volume UUID is the directory name under /storage.
        val mounted = File("/storage/$volumeId")
        return if (mounted.isDirectory) mounted else null
    }

    private fun containedFolder(root: File, folder: File): File? {
        val canonicalRoot = canonical(root)
        val canonicalFolder = canonical(folder)
        val rootPath = canonicalRoot.path
        val folderPath = canonicalFolder.path
        if (folderPath == rootPath) return canonicalFolder
        if (!folderPath.startsWith(rootPath + File.separator)) return null
        return canonicalFolder
    }

    private fun canonical(file: File): File =
        runCatching { file.canonicalFile }.getOrDefault(file.absoluteFile)

    private const val EXTERNAL_STORAGE_AUTHORITY =
        "com.android.externalstorage.documents"
    private const val PRIMARY_VOLUME = "primary"
    private const val RAW_PREFIX = "raw:"
}
