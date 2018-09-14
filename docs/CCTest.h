
#if 0
    /// BPS probe offset currently in use
    unsigned BPSProbeOffset = protocol::kInitialBPSProbeOffset;

    /// Next BPS target to hit while probing
    unsigned BPSProbeTarget = 0;

    /// Target BPS for delay probe
    unsigned DelayProbeTargetBPS = 0;

    /// Last delay seen while below delay probe BPS rate
    unsigned DelayProbeLastUsec = 0;

    /// Current state
    enum class StateMachine
    {
        BandwidthProbe,
        DelayProbe
    };
    StateMachine State = StateMachine::BandwidthProbe;

    /// Maximum bandwidth value within a window
    siamese::WindowedMinMax< unsigned, siamese::WindowedMaxCompare<unsigned> > MaxWindowedBW;

    /// Minimum delay value within a window
    siamese::WindowedMinMax< unsigned, siamese::WindowedMinCompare<unsigned> > MinWindowedDelay;
#endif
