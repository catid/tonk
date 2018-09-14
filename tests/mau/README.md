## Mau : UDP Network Simulator in C

Mau.h provides a simple C API for a powerful, multithreaded network sim.
Designed for real-time unit testing and performance testing applications,
Mau provides a proxy UDP port that simulates some network conditions.

Mau is provided with the server hostname and UDP port via its API.
The client address is determined from the first UDP datagram received.
Packets from the client are forwarded to the server with added delay,
packetloss, etc.  Packets from the server are forwarded to the client
with the same channel configuration.

Knobs supported so far:
+ Packetloss rate (uniform dist).
+ Minimum added latency (within 1 millisecond precision).

It is written for low-overhead so that hundreds of tests can be run in
parallel in real-time over the localhost loopback.  I wrote this
primarily to help improve testing for my own reliable UDP library.

An attempt was made at portability, though it may need some small tweaks
to run on Mac or Linux right now.  CMake is used for its makefile.
Mau uses the Asio library for portable networking and strands.

### Example Usage:

    MauChannelConfig channel;
    channel.MinimumLatencyMsec = 100;
    channel.MinimumPacketlossRate = 0.01;
    channel.RNGSeed = 1;

    MauProxyConfig config;
    config.UDPListenPort = 10200;
    const char* hostname = "localhost";
    uint16_t port = 5060;
    
    MauProxy proxy;
    MauResult createResult = mau_proxy_create(
        &config,
        &channel,
        hostname,
        port,
        &proxy
    );
    if (MAU_FAILED(createResult))
    {
        Logger.Error("mau_proxy_destroy failed: ", createResult);
        return false;
    }

    Logger.Debug("Press ENTER key to stop client");
    ::getchar();
    Logger.Debug("...Key press detected.  Stopping..");

    MauResult destroyResult = mau_proxy_destroy(
        proxy
    );
