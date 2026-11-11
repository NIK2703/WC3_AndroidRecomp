package wc3.recomp.launch

import android.content.Context
import android.net.Uri
import android.os.Build
import org.json.JSONException
import org.json.JSONObject
import java.io.File
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.io.RandomAccessFile
import java.security.MessageDigest
import java.util.Locale
import java.util.UUID
import java.util.zip.ZipFile

/**
 * ADPkg importer and app-private driver registry (ADR-036, DRV-1..DRV-13).
 *
 * The installer owns the ZIP/JSON/SHA-256 layer.  The native loader re-checks
 * driver.ini and the ELF header before it calls libadrenotools, so a package
 * that is modified after install is refused instead of loaded.
 */
class VulkanDriverStore(context: Context) {
    private val app = context.applicationContext
    val root: File = File(app.filesDir, "drivers")

    data class DriverInfo(
        val id: String,
        val name: String,
        val vendor: String,
        val driverVersion: String,
        val minApi: Int,
        val size: Long,
        val library: String,
        val installedAt: Long
    ) {
        val displayName: String
            get() = if (vendor.isBlank()) name else "$name ($vendor)"
    }

    sealed class InstallResult {
        data class Ok(val info: DriverInfo) : InstallResult()
        data class Error(val kind: ErrorKind, val detail: String = "") : InstallResult()
    }

    enum class ErrorKind {
        OPEN, ZIP, META, LIBRARY, ELF, IO, API, DUPLICATE
    }

    private class Reject(val kind: ErrorKind, val detail: String = "") : Exception(detail)

    init {
        cleanupStaging()
    }

    fun list(): List<DriverInfo> {
        root.mkdirs()
        val result = ArrayList<DriverInfo>()
        val dirs = root.listFiles() ?: return result
        for (dir in dirs) {
            if (!dir.isDirectory || dir.name.startsWith(".")) continue
            if (!ID.matches(dir.name)) continue
            if (!File(dir, ".installed").isFile) continue
            result += readInfo(dir) ?: continue
        }
        return result.sortedWith(compareByDescending { it.installedAt })
    }

    fun find(id: String): DriverInfo? {
        if (!ID.matches(id)) return null
        val dir = File(root, id)
        if (!dir.isDirectory || !File(dir, ".installed").isFile) return null
        return readInfo(dir)
    }

    fun install(uri: Uri): InstallResult {
        root.mkdirs()
        val stagingRoot = File(root, ".staging")
        stagingRoot.mkdirs()
        val staging = File(stagingRoot, UUID.randomUUID().toString())
        var temp: File? = null
        var zip: ZipFile? = null
        try {
            val tempFile = File.createTempFile("adpkg-", ".zip", app.cacheDir)
            temp = tempFile
            val source = copyUriToFile(uri, tempFile)
            zip = ZipFile(tempFile)
            val entries = zip.entries().toList()
            if (entries.size > MAX_ENTRIES) throw Reject(ErrorKind.ZIP)
            var total = 0L
            var metaCount = 0
            var libraryCount = 0
            for (entry in entries) {
                val name = entry.name
                if (entry.isDirectory) throw Reject(ErrorKind.ZIP, name)
                if (!SAFE_NAME.matches(name)) throw Reject(ErrorKind.ZIP, name)
                if (name == "meta.json") {
                    metaCount++
                } else if (name.endsWith(".so")) {
                    // customDriverDir is the first namespace search path, so a
                    // package-local hook would shadow the app's own hook.
                    if (name in RESERVED_HOOKS) throw Reject(ErrorKind.LIBRARY, name)
                    libraryCount++
                } else {
                    throw Reject(ErrorKind.ZIP, name)
                }
                if (entry.size > MAX_ENTRY_SIZE) throw Reject(ErrorKind.ZIP, name)
                total += maxOf(0L, entry.size)
                if (total > MAX_TOTAL_SIZE) throw Reject(ErrorKind.ZIP)
            }
            if (metaCount != 1 || libraryCount == 0) throw Reject(ErrorKind.ZIP)

            val metaEntry = zip.getEntry("meta.json")
                ?: throw Reject(ErrorKind.META, "meta.json")
            if (metaEntry.size > MAX_META_SIZE) throw Reject(ErrorKind.META)
            val meta = parseMeta(readSmall(zip, metaEntry, MAX_META_SIZE))
            if (meta.minApi < 1 || meta.minApi > 1000) throw Reject(ErrorKind.META)
            if (!SAFE_NAME.matches(meta.libraryName) ||
                !meta.libraryName.endsWith(".so")
            ) throw Reject(ErrorKind.LIBRARY, meta.libraryName)
            if (zip.getEntry(meta.libraryName) == null)
                throw Reject(ErrorKind.LIBRARY, meta.libraryName)
            if (Build.VERSION.SDK_INT < maxOf(meta.minApi, MIN_API))
                throw Reject(ErrorKind.API, meta.minApi.toString())

            if (!staging.mkdirs()) throw Reject(ErrorKind.IO, "staging")
            val digest = MessageDigest.getInstance("SHA-256")
            var mainSize = 0L
            var mainPage16k = false
            var extractedTotal = 0L
            for (entry in entries) {
                val out = File(staging, entry.name)
                val isMain = entry.name == meta.libraryName
                val written = extract(zip, entry, out, if (isMain) digest else null)
                extractedTotal += written
                if (extractedTotal > MAX_TOTAL_SIZE) throw Reject(ErrorKind.ZIP)
                if (entry.compressedSize > 0L &&
                    written / entry.compressedSize > MAX_RATIO
                ) throw Reject(ErrorKind.ZIP, entry.name)
                if (isMain) {
                    mainSize = written
                    val elf = checkElf(out) ?: throw Reject(ErrorKind.ELF)
                    mainPage16k = elf
                }
            }
            if (mainSize <= 0L) throw Reject(ErrorKind.LIBRARY)

            val sha = hex(digest.digest())
            val id = sha.substring(0, 16)
            val installedAt = System.currentTimeMillis() / 1000L
            File(staging, "source.txt").writeText(
                buildString {
                    append("name=").append(uri.lastPathSegment.orEmpty()).append('\n')
                    append("size=").append(source.size).append('\n')
                    append("sha256=").append(source.sha256).append('\n')
                },
                Charsets.US_ASCII
            )
            File(staging, "driver.ini").writeText(
                buildString {
                    append("id=").append(id).append('\n')
                    append("library=").append(meta.libraryName).append('\n')
                    append("min_api=").append(meta.minApi).append('\n')
                    append("sha256=").append(sha).append('\n')
                    append("size=").append(mainSize).append('\n')
                    append("page16k=").append(if (mainPage16k) 1 else 0).append('\n')
                    append("installed_at=").append(installedAt).append('\n')
                },
                Charsets.US_ASCII
            )
            // The marker is written last: a directory without it is never
            // visible to the loader or to list().
            File(staging, ".installed").writeBytes(ByteArray(0))

            val target = File(root, id)
            if (target.exists()) {
                staging.deleteRecursively()
                return find(id)?.let { InstallResult.Ok(it) }
                    ?: InstallResult.Error(ErrorKind.IO, id)
            }
            if (!staging.renameTo(target)) {
                throw Reject(ErrorKind.IO, "rename")
            }
            val info = readInfo(target) ?: throw Reject(ErrorKind.IO, id)
            return InstallResult.Ok(info)
        } catch (reject: Reject) {
            return InstallResult.Error(reject.kind, reject.detail)
        } catch (zipError: java.util.zip.ZipException) {
            return InstallResult.Error(ErrorKind.ZIP, zipError.message.orEmpty())
        } catch (io: IOException) {
            return InstallResult.Error(ErrorKind.IO, io.message.orEmpty())
        } catch (json: JSONException) {
            return InstallResult.Error(ErrorKind.META, json.message.orEmpty())
        } catch (security: SecurityException) {
            return InstallResult.Error(ErrorKind.OPEN, security.message.orEmpty())
        } finally {
            try {
                zip?.close()
            } catch (_: IOException) {
            }
            temp?.delete()
            if (staging.exists()) staging.deleteRecursively()
        }
    }

    fun remove(id: String): Boolean {
        if (!ID.matches(id)) return false
        return try {
            val dir = File(root, id)
            if (!File(dir, ".installed").isFile) return false
            val canonRoot = root.canonicalFile
            val canonDir = dir.canonicalFile
            canonDir.parentFile == canonRoot && dir.deleteRecursively()
        } catch (_: IOException) {
            false
        }
    }

    private fun copyUriToFile(uri: Uri, dst: File): SourceInfo {
        val digest = MessageDigest.getInstance("SHA-256")
        val input = app.contentResolver.openInputStream(uri)
            ?: throw Reject(ErrorKind.OPEN, uri.toString())
        input.use { source ->
            dst.outputStream().use { output ->
                val copied = copyBounded(source, output, MAX_ZIP_SIZE, digest)
                if (copied <= 0L) throw Reject(ErrorKind.OPEN)
                return SourceInfo(copied, hex(digest.digest()))
            }
        }
    }

    private fun extract(
        zip: ZipFile,
        entry: java.util.zip.ZipEntry,
        dst: File,
        digest: MessageDigest?
    ): Long {
        val input = zip.getInputStream(entry)
        input.use { source ->
            dst.outputStream().use { output ->
                return copyBounded(source, output, MAX_ENTRY_SIZE, digest)
            }
        }
    }

    private fun copyBounded(
        source: InputStream,
        output: OutputStream,
        cap: Long,
        digest: MessageDigest?
    ): Long {
        val buf = ByteArray(64 * 1024)
        var total = 0L
        while (true) {
            val n = source.read(buf)
            if (n < 0) break
            total += n
            if (total > cap) throw Reject(ErrorKind.ZIP)
            digest?.update(buf, 0, n)
            output.write(buf, 0, n)
        }
        return total
    }

    private fun readSmall(zip: ZipFile, entry: java.util.zip.ZipEntry, cap: Long): String {
        val input = zip.getInputStream(entry)
        input.use {
            val buf = ByteArray(cap.toInt() + 1)
            var used = 0
            while (used < buf.size) {
                val n = it.read(buf, used, buf.size - used)
                if (n < 0) break
                used += n
            }
            if (used > cap) throw Reject(ErrorKind.META)
            return String(buf, 0, used, Charsets.UTF_8)
        }
    }

    private fun parseMeta(text: String): Meta {
        val json = JSONObject(text)
        fun requiredString(key: String, max: Int): String {
            if (!json.has(key) || json.isNull(key))
                throw Reject(ErrorKind.META, key)
            val value = json.optString(key, "").trim()
            if (value.isEmpty() || value.length > max)
                throw Reject(ErrorKind.META, key)
            return value
        }

        if (json.optInt("schemaVersion", -1) != 1)
            throw Reject(ErrorKind.META, "schemaVersion")
        val description = json.optString("description", "")
        if (description.length > 512) throw Reject(ErrorKind.META, "description")
        requiredString("author", 128)
        requiredString("packageVersion", 64)
        val minApi = try {
            json.getInt("minApi")
        } catch (_: JSONException) {
            throw Reject(ErrorKind.META, "minApi")
        }
        return Meta(
            name = requiredString("name", 128),
            vendor = requiredString("vendor", 32),
            driverVersion = requiredString("driverVersion", 64),
            minApi = minApi,
            libraryName = requiredString("libraryName", 128)
        )
    }

    private fun readInfo(dir: File): DriverInfo? {
        val ini = readIni(File(dir, "driver.ini")) ?: return null
        val id = ini["id"] ?: return null
        if (id != dir.name || !ID.matches(id)) return null
        val library = ini["library"] ?: return null
        val size = ini["size"]?.toLongOrNull() ?: 0L
        val installedAt = ini["installed_at"]?.toLongOrNull() ?: 0L
        val meta = try {
            val text = File(dir, "meta.json").takeIf { it.isFile }?.readText()
                ?: "{}"
            parseMeta(text)
        } catch (_: Exception) {
            Meta(id, "", "", 0, library)
        }
        return DriverInfo(
            id = id,
            name = meta.name,
            vendor = meta.vendor,
            driverVersion = meta.driverVersion,
            minApi = meta.minApi,
            size = size,
            library = library,
            installedAt = installedAt
        )
    }

    private fun readIni(file: File): Map<String, String>? {
        if (!file.isFile) return null
        return try {
            val values = HashMap<String, String>()
            file.readLines(Charsets.US_ASCII).forEach { raw ->
                val line = raw.trim()
                if (line.isEmpty() || line.startsWith("#")) return@forEach
                val eq = line.indexOf('=')
                if (eq <= 0) return@forEach
                values[line.substring(0, eq)] = line.substring(eq + 1)
            }
            values
        } catch (_: IOException) {
            null
        }
    }

    /** Returns the 16 KiB-page requirement, or null if this is not an
     * AArch64 ET_DYN shared object. */
    private fun checkElf(file: File): Boolean? {
        if (file.length() < 64L || file.length() > 0xFFFFFFFFL) return null
        RandomAccessFile(file, "r").use { raf ->
            val eh = ByteArray(64)
            raf.readFully(eh)
            if (eh[0] != 0x7f.toByte() || eh[1] != 0x45.toByte() ||
                eh[2] != 0x4c.toByte() || eh[3] != 0x46.toByte()
            ) return null
            if (eh[4] != 2.toByte() || eh[5] != 1.toByte()) return null
            val type = le16(eh, 16)
            val machine = le16(eh, 18)
            val phoff = le64(eh, 32)
            val phentsize = le16(eh, 54)
            val phnum = le16(eh, 56)
            if (type != 3 || machine != 183 || phentsize != 56 ||
                phnum <= 0 || phnum > 256 ||
                phoff < 0 || phoff + phnum.toLong() * 56L > file.length()
            ) return null
            raf.seek(phoff)
            var load = false
            var dynamic = false
            var page16k = false
            val ph = ByteArray(56)
            repeat(phnum) {
                raf.readFully(ph)
                when (le32(ph, 0)) {
                    1 -> {
                        load = true
                        if (le64(ph, 48) >= 16384L) page16k = true
                    }
                    2 -> dynamic = true
                }
            }
            if (!load || !dynamic) return null
            return page16k
        }
    }

    private fun cleanupStaging() {
        val cutoff = System.currentTimeMillis() - 60L * 60L * 1000L
        File(root, ".staging").listFiles()?.forEach { file ->
            if (file.lastModified() < cutoff) file.deleteRecursively()
        }
        // Remove the pre-rev1 flat staging names as well.
        root.listFiles()?.forEach { file ->
            if (file.name.startsWith(".staging-")) file.deleteRecursively()
        }
    }

    private fun hex(bytes: ByteArray): String = bytes.joinToString("") {
        String.format(Locale.US, "%02x", it.toInt() and 0xff)
    }

    private fun le16(buf: ByteArray, at: Int): Int =
        (buf[at].toInt() and 0xff) or ((buf[at + 1].toInt() and 0xff) shl 8)

    private fun le32(buf: ByteArray, at: Int): Int =
        le16(buf, at) or (le16(buf, at + 2) shl 16)

    private fun le64(buf: ByteArray, at: Int): Long =
        (le32(buf, at).toLong() and 0xffffffffL) or
            (le32(buf, at + 4).toLong() shl 32)

    private data class Meta(
        val name: String,
        val vendor: String,
        val driverVersion: String,
        val minApi: Int,
        val libraryName: String
    )

    private data class SourceInfo(val size: Long, val sha256: String)

    companion object {
        private val ID = Regex("^[0-9a-f]{16}$")
        private val SAFE_NAME = Regex("^[A-Za-z0-9][A-Za-z0-9._-]*$")
        private const val MIN_API = 28
        private const val MAX_ENTRIES = 8
        private const val MAX_META_SIZE = 16L * 1024L
        private const val MAX_ENTRY_SIZE = 96L * 1024L * 1024L
        private const val MAX_TOTAL_SIZE = 192L * 1024L * 1024L
        private const val MAX_ZIP_SIZE = 192L * 1024L * 1024L
        private const val MAX_RATIO = 200L

        /**
         * libadrenotools needs the two hook DSOs as real files on a linker
         * namespace search path.  Modern packaging keeps jniLibs uncompressed
         * inside the APK, so this extracts only those two small DSOs into
         * app-private storage; the product .so is never extracted (O-12).
         *
         * The directory is keyed by the installed package revision so an
         * update never reuses hooks from an older APK.  An empty result means
         * the native loader reports 4004/4005 and falls back to the system
         * driver instead of loading a half-materialised hook set.
         */
        @JvmStatic
        fun ensureHookDir(context: Context): String {
            val app = context.applicationContext
            val sourceDir = app.applicationInfo.sourceDir ?: return ""
            val apk = File(sourceDir)
            val base = File(app.filesDir, "adrenotools-hooks")
            val revision = try {
                val info = app.packageManager.getPackageInfo(app.packageName, 0)
                "${info.versionCode}-${info.lastUpdateTime}-${apk.length()}-${apk.lastModified()}"
            } catch (_: Exception) {
                "${apk.length()}-${apk.lastModified()}"
            }
            val dest = File(base, revision)
            if (hasHooks(dest)) return dest.absolutePath
            if (!dest.mkdirs() && !dest.isDirectory) return ""

            try {
                ZipFile(apk).use { zip ->
                    for (name in HOOKS) {
                        val entry = zip.getEntry("lib/arm64-v8a/$name")
                            ?: return ""
                        val out = File(dest, name)
                        val tmp = File(dest, "$name.tmp")
                        zip.getInputStream(entry).use { input ->
                            tmp.outputStream().use { output ->
                                input.copyTo(output, 64 * 1024)
                            }
                        }
                        if (!tmp.renameTo(out)) {
                            tmp.delete()
                            if (!out.isFile || out.length() == 0L) return ""
                        }
                    }
                }
            } catch (_: IOException) {
                return ""
            }
            if (!hasHooks(dest)) return ""

            // Keep only the revision that is about to be used.
            base.listFiles()?.forEach { old ->
                if (old.absolutePath != dest.absolutePath) old.deleteRecursively()
            }
            return dest.absolutePath
        }

        private fun hasHooks(dir: File): Boolean =
            HOOKS.all { File(dir, it).let { f -> f.isFile && f.length() > 0L } }

        private val HOOKS = listOf("libmain_hook.so", "libhook_impl.so")
        private val RESERVED_HOOKS = HOOKS.toSet()
    }
}
