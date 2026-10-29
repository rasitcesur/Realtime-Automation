package alcybe.realtime;

/**
 * Reusable container holding scheduling metadata without heap churn.
 */
public final class RealtimeThreadContainer {

    public RealtimeThread thread;
    public long timeOffsetNs;
    public long cycleTimeNs;

    public RealtimeThreadContainer() {}

    public void set(RealtimeThread thread, long timeOffsetNs, long cycleTimeNs) {
        this.thread = thread;
        this.timeOffsetNs = timeOffsetNs;
        this.cycleTimeNs = cycleTimeNs;
    }

    public void clear() {
        this.thread = null;
        this.timeOffsetNs = 0;
        this.cycleTimeNs = 0;
    }
}