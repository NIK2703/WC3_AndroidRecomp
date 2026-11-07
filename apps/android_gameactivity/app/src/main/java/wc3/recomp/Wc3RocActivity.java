package wc3.recomp;

import android.os.Bundle;

/**
 * Reign of Chaos launcher.
 *
 * <p>The Windows Reign of Chaos launcher starts {@code war3.exe} with
 * {@code -classic}; this activity selects that mode before GameActivity starts
 * the native thread.  It runs in its own Android process so it never shares a
 * live guest with the Frozen Throne activity.</p>
 */
public final class Wc3RocActivity extends Wc3GameActivity {
    @Override
    protected void onCreate(final Bundle savedInstanceState) {
        setGameMode("roc");
        super.onCreate(savedInstanceState);
    }
}
