package alcybe.realtime;

/** Loads the native JNI implementation exactly once per class loader. */
public final class RealtimeInitializer {
    private RealtimeInitializer() {}

    public static void init() { Holder.LOADED.toString(); }

    private static final class Holder {
        static final Boolean LOADED;
        static {
            System.loadLibrary("realtime");
            LOADED = Boolean.TRUE;
        }
    }
}
