package wc3.recomp.launch

/**
 * The three mutually exclusive touch schemes exposed by the product settings
 * screen.  Keeping this as one enum prevents the old two-boolean state where
 * "single tap" and "direct touch" both looked like `doubleTapGestures=false`.
 */
enum class GestureMode(val id: String) {
    DOUBLE_TAP("double_tap"),
    SINGLE_TAP("single_tap"),
    DIRECT_TOUCH("direct_touch");

    /** The legacy overlay flag written to the native host. */
    val doubleTapGestures: Boolean
        get() = this == DOUBLE_TAP

    /** True for the semantic direct-touch variant; the native side also
     * requires a guest sink before any field touch leaves the overlay. */
    val directTouch: Boolean
        get() = this == DIRECT_TOUCH

    companion object {
        fun fromId(value: String?): GestureMode? =
            entries.firstOrNull { it.id == value }
    }
}
