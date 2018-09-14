
#if 0
    // Calculate instantaneous statistics
    if (!Instant.UpdateOnDatagram(datagram))
        return; // No data
    InstantStats instant = Instant.GetStats();
    if (instant.NetworkTripUsec <= 0 || instant.ReceiveBPS <= 0)
        return; // No data - Wait for first time sync message (should be first round-trip)

    // Pick a window interval
    static const unsigned kMaxWindowInterval = 2000 * 1000; ///< 2 seconds
    unsigned windowUsec = Statistics.GetTripUsec() * 4;
    if (windowUsec > kMaxWindowInterval)
        windowUsec = kMaxWindowInterval;

    if (IncreasingBPS)
    {
        if (instant.NetworkTripUsec < Params.MaximumDelayMsec * 1000)
        {
            unsigned slowStart = (unsigned)(instant.SendBPS * 1.1);

            //if (TargetBPS < slowStart)
                TargetBPS = slowStart;
        }
        else
        {
            IncreasingBPS = false;

            MinWindowedDelay.Reset();
            MinWindowedDelay.Update(instant.NetworkTripUsec, datagram.PostProcessUsec, windowUsec);

            unsigned slowStart = (unsigned)(instant.SendBPS * 0.9);

            if (TargetBPS > slowStart)
                TargetBPS = slowStart;
        }
    }
    else
    {
        if (instant.NetworkTripUsec < MinWindowedDelay.GetBest())
        {
            MinWindowedDelay.Update(instant.NetworkTripUsec, datagram.PostProcessUsec, windowUsec);

            unsigned slowStart = (unsigned)(instant.SendBPS * 0.9);

            if (TargetBPS > slowStart)
                TargetBPS = slowStart;
        }
        else
        {
            IncreasingBPS = true;

            unsigned slowStart = (unsigned)(instant.SendBPS * 1.1);

            if (TargetBPS < slowStart)
                TargetBPS = slowStart;
        }
    }

    TONK_VERBOSE_BANDWIDTH_LOG("BW: IncreasingBPS=", IncreasingBPS, " SendBPS=", instant.SendBPS, " TargetBPS=", TargetBPS);

    Shape.AverageBPS = TargetBPS;
#endif

#if 0
    // If received much less than sent:
    if ((unsigned)(instant.ReceiveBPS * 1.1f) < instant.SendBPS)
    {
        TargetBPS = (unsigned)(instant.ReceiveBPS * 0.9f); // Decrease rate
        TONK_VERBOSE_BANDWIDTH_LOG("BW: ReceiveBPS=", instant.ReceiveBPS, " < SendBPS=", instant.SendBPS, ". Decreasing rate = ", TargetBPS);
    }
    // Else if delay is blowing up:
    else if (instant.NetworkTripUsec >= Params.MaximumDelayMsec * 1000)
    {
        TargetBPS = (unsigned)(instant.ReceiveBPS * 0.9f); // Decrease rate
        TONK_VERBOSE_BANDWIDTH_LOG("BW: NetworkTripUsec=", instant.NetworkTripUsec, ". Decreasing rate = ", TargetBPS);
    }
    // Else if queue is flushing right now
    else if (instant.ReceiveBPS > (unsigned)(instant.SendBPS * 1.1f))
    {
        // Keep flushing
        TargetBPS = instant.SendBPS;
        TONK_VERBOSE_BANDWIDTH_LOG("BW: ReceiveBPS=", instant.ReceiveBPS, " > SendBPS=", instant.SendBPS, ". Maintaining rate = ", TargetBPS);
    }
    else // Else send rate is about the same as receive rate
    {
        // Try increasing
        unsigned increasedRate = (unsigned)(instant.ReceiveBPS * 1.1f) + 10000;
        if (TargetBPS < increasedRate)
            TargetBPS = increasedRate;
        TONK_VERBOSE_BANDWIDTH_LOG("BW: ReceiveBPS=", instant.ReceiveBPS, " = SendBPS=", instant.SendBPS, ". Increasing rate = ", TargetBPS);
    }

    Shape.AverageBPS = TargetBPS;
#endif

#if 0
    unsigned bps;

    // If delay is increasing, switch to delay probe
    unsigned shortestTripUsec  = MinWindowedDelay.GetBest();
    unsigned thresholdTripUsec = (shortestTripUsec * 5) / 4; // 1.25 * minimum
    if (shortestTripUsec > 0 && instant.NetworkTripUsec > thresholdTripUsec)
        State = StateMachine::DelayProbe;
    if (instant.NetworkTripUsec < protocol::kMinimumDelayProbeUsec)
        State = StateMachine::BandwidthProbe;

    // If probing delay:
    if (State == StateMachine::DelayProbe)
    {
        // Start with the current bandwidth
        bps = instant.ReceiveBPS;

        // Calculate target trip
        unsigned targetTripUsec = shortestTripUsec;
        if (DelayProbeLastUsec > 0 && targetTripUsec < DelayProbeLastUsec)
            targetTripUsec = (instant.NetworkTripUsec * 3) / 4;

        // This is above BPS limit
        unsigned extraUsec = instant.NetworkTripUsec - targetTripUsec;
        unsigned bytesQueue = (unsigned)(((uint64_t)instant.ReceiveBPS * extraUsec) / 1000000);

        // Reduce BPS to flush queue over the next 500 ms
        bytesQueue *= 2;
        if (bps <= bytesQueue + protocol::kMinimumBytesPerSecond)
            bps = protocol::kMinimumBytesPerSecond;
        else
            bps -= bytesQueue;

        // If we have not hit the delay probe target BPS yet:
        if (instant.ReceiveBPS > DelayProbeTargetBPS)
        {
            // Update the probe target BPS
            DelayProbeTargetBPS = bps;
            DelayProbeLastUsec  = instant.NetworkTripUsec;
        }
        else if (instant.NetworkTripUsec > DelayProbeLastUsec)
            State = StateMachine::BandwidthProbe;

        // Reset BPS probe
        BPSProbeTarget = 0;
        BPSProbeOffset = protocol::kInitialBPSProbeOffset;

        TONK_VERBOSE_BANDWIDTH_LOG("Above threshold! BytesPerSecond=", bps,
            " NetworkTripUsec=", instant.NetworkTripUsec,
            " thresholdTripUsec=", thresholdTripUsec,
            " extraUsec=", extraUsec,
            " bytesQueue=", bytesQueue);
    }
    else
    {
        // Continuously estimate maximum bandwidth that does not increase delay
        MaxWindowedBW.Update(instant.ReceiveBPS, datagram.PostProcessUsec, windowUsec);
        bps = MaxWindowedBW.GetBest();

        // Add new probe offset
        bps += BPSProbeOffset;

        // If BPS probe target was hit:
        if (bps >= BPSProbeTarget)
        {
            // Update next probe target
            BPSProbeTarget = bps + BPSProbeOffset;

            // Push 2x harder
            BPSProbeOffset *= 2;
        }

        // Update minimum delay seen before delay kicks up
        MinWindowedDelay.Update(instant.NetworkTripUsec, datagram.PostProcessUsec, windowUsec);

        // Reset delay probe
        DelayProbeTargetBPS = 0;
        DelayProbeLastUsec = 0;

        TONK_VERBOSE_BANDWIDTH_LOG("Below delayThresh: BPS=", bps, " BPSProbeTarget=", BPSProbeTarget);
    }

    // Update BPS
    if (bps < protocol::kMinimumBytesPerSecond)
        bps = protocol::kMinimumBytesPerSecond;
    Shape.AverageBPS = bps;
#endif
