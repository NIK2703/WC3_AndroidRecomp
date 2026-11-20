package wc3.recomp.launch

import java.io.File
import java.util.Locale

/** One CPU that the current process is allowed to run on. */
data class CpuCore(
    val id: Int,
    val label: String,
    val maxFrequencyMhz: Int? = null,
    val capacity: Int? = null
)

/**
 * Reads the CPU affinity of this process, not the device-wide online count.
 * Android can restrict an app to a cpuset; pinning a worker to a CPU outside
 * that set fails with EINVAL, so the settings screen must only offer the CPUs
 * the game process can actually use.
 */
object CpuTopology {
    /* The native affinity syscall receives a 64-bit CPU mask.  Shared by the
     * settings table and the launch bundle so the cap has one owner. */
    internal const val MAX_CPU_ID = 63
    private const val STATUS_PATH = "/proc/self/status"
    private const val CPU_ROOT = "/sys/devices/system/cpu"

    fun availableCores(): List<CpuCore> =
        allowedCpuIds().map { id ->
            val mhz = maxFrequencyMhz(id)
            val label = if (mhz != null) {
                String.format(Locale.US, "CPU %d · %.1f GHz", id, mhz / 1000.0)
            } else {
                "CPU $id"
            }
            CpuCore(id, label, mhz, cpuCapacity(id))
        }

    /**
     * Picks the most performant core from an already filtered list.  Kernel
     * `cpu_capacity` is the primary metric when every core reports it; else
     * fall back to `cpuinfo_max_freq`, then to the highest id (big cores
     * normally have the highest ids on ARM).
     */
    fun pickBest(cores: List<CpuCore>): CpuCore? {
        if (cores.isEmpty()) return null
        val useCapacity = cores.all { (it.capacity ?: 0) > 0 }
        return cores.maxWithOrNull(
            if (useCapacity) {
                compareBy<CpuCore> { it.capacity ?: 0 }
                    .thenBy { it.maxFrequencyMhz ?: -1 }
                    .thenBy { it.id }
            } else {
                compareBy<CpuCore> { it.maxFrequencyMhz ?: -1 }
                    .thenBy { it.id }
            }
        )
    }

    private fun allowedCpuIds(): List<Int> {
        val line = try {
            File(STATUS_PATH).useLines { lines ->
                lines.firstOrNull { it.startsWith("Cpus_allowed_list:") }
            }
        } catch (_: Exception) {
            null
        }
        val parsed = line
            ?.substringAfter(':')
            ?.let(::parseCpuList)
            .orEmpty()
        if (parsed.isNotEmpty()) return parsed

        /* Fallback for a kernel that hides the status line: the count is not
         * the highest CPU id, but it keeps the screen usable.  Both the native
         * 64-bit mask and the product table stop at CPU 63. */
        val count = Runtime.getRuntime().availableProcessors()
            .coerceIn(1, MAX_CPU_ID + 1)
        return (0 until count).toList()
    }

    internal fun parseCpuList(raw: String): List<Int> {
        val ids = sortedSetOf<Int>()
        for (part in raw.trim().split(',')) {
            val token = part.trim()
            if (token.isEmpty()) continue
            val dash = token.indexOf('-')
            if (dash < 0) {
                token.toIntOrNull()?.let { if (it in 0..MAX_CPU_ID) ids.add(it) }
                continue
            }
            val first = token.substring(0, dash).trim().toIntOrNull() ?: continue
            val last = token.substring(dash + 1).trim().toIntOrNull() ?: continue
            if (first > MAX_CPU_ID || last < 0) continue
            val low = maxOf(minOf(first, last), 0)
            val high = minOf(maxOf(first, last), MAX_CPU_ID)
            if (low > high) continue
            for (id in low..high) ids.add(id)
        }
        return ids.toList()
    }

    private fun maxFrequencyMhz(id: Int): Int? {
        val khz = readLong("$CPU_ROOT/cpu$id/cpufreq/cpuinfo_max_freq")
            ?: return null
        if (khz <= 0L) return null
        return (khz / 1000L).toInt()
    }

    private fun cpuCapacity(id: Int): Int? {
        val value = readLong("$CPU_ROOT/cpu$id/cpu_capacity")
            ?: readLong("$CPU_ROOT/cpu$id/topology/core_capacity")
            ?: return null
        if (value <= 0L || value > Int.MAX_VALUE) return null
        return value.toInt()
    }

    private fun readLong(path: String): Long? =
        try {
            File(path).readText().trim().toLongOrNull()
        } catch (_: Exception) {
            null
        }
}
