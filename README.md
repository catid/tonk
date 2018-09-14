# Tonk
## Reliable UDP (rUDP) Network Library in C++

Tonk.h provides a simple C API for a powerful, multithreaded network engine.
The Tonk network engine transports message data over the Internet using a
custom protocol over UDP/IP sockets.

#### Tonk is experimental: It's only been unit tested and hasn't been used for any major projects yet.  There are probably bugs to fix.  It currently only builds for Linux and Windows targets right now.

Tonk puts each connection into its own thread, so all processing related to
each connection happens serially.  The application is responsible for
handling thread safety on the server side between multiple connections.

Tonk implements a new type of jitter-robust time synchronization protocol
over the Internet that is more suitable for mobile networks than NTP/PTP.
Tonk enables applications to compress timestamps down to 2 or 3 bytes.

Tonk implements a novel HARQ transport based on SiameseFEC, meaning that a
new type of forward error correction is used to protect data and avoid
head-of-line blocking that causes latency spikes with TCP sockets.
SiameseFEC is the world's first infinite-window erasure code, suitable for
protecting reliable in-order data like a TCP stream, but less suitable for
audio or video data protection, which does not need an infinite window.

Tonk implements a new type of congestion control strategy that attempts to
guarantee low latency for real-time data while full-speed file transfers
are being performed in the background.  Tonk supports three levels of
message prioritization, 12 different parallel data streams, unordered
delivery, and unmetered unreliable delivery.

Tonk implements several types of fully automated Peer2Peer NAT traversal to
allow mobile applications to peer with the aid of a rendezvous server.
It also incorporates WLANOptimizer to reduce latency on some Windows PCs.

Tonk's philosophy is to trade extra CPU work and extra memory for faster
data delivery, so it may not be suitable for your application.  For example
it uses streaming data compression with Zstd to compress packets so there is
less to send, but this does take more CPU resources and requires memory.
It also implements the fastest known erasure codes for this application and
it has to send recovery packets in addition to normal data, which takes more
CPU/network resources but will avoid head-of-line blocking and deliver the
data faster.  It uses a multithreaded design, which requires extra complexity
and overhead but means that multiple connections can be processed in parallel
to reduce the overall latency.  The peer2peer design allows the application
to cut out a network hop to the server and this requires a lot of additional
complexity, but the result is significantly lower latency.  At the same time
to offset all of these additional costs, a lot of engineering effort went
into choosing/designing/tuning the best algorithms to minimize overhead.
TL;DR Tonk is all about reducing latency by taking advantage of faster CPUs.


### Usage

CMake can be used to generate build files for Linux or Windows.

The `tonk` project is a dynamic link library (DLL) providing the Tonk C API.
The `tonk_static` project is a static linkage version of Tonk.

There is also a `tonkcppsdk` project that builds a C++ static library containing
a loader that will load the Tonk DLL and also defines the C++ SDK for Tonk.

There is a LAN advertising and direct file transfer demo in the `FileSender` and `FileReceiver` projects.

There's a Peer2Peer file transfer demo as well.  The `P2PFileRendezvousServer` project contains the server that must be on a public server.  You can provide the DNS hostname and file names as arguments to the `P2PFileReceiver` and `P2PFileSender` applications.  These demo applications will establish a peer2peer connection via the rendezvous server and transfer a file while reporting RTT and OWD conditions periodically.

The `test_server` and `test_p2p_client` projects are unit test projects that may have some useful example code.

There is a unit tester that uses MauProxy to simulate a file transfer during all
sorts of nasty network conditions (packetloss, re-ordering, corruption, etc),
which also tests a bunch of the classes.


### Appropriate applications:

+ Data rate limit of 20 MB/s per connection
+ Low-latency messaging
+ Mobile file (e.g. video/image) upload
+ Lossless video/audio streaming
+ Real-time multiplayer gaming
+ Mobile VPN data accelerator
+ Peer2Peer mobile file transfer, chat, or data streaming

### Inappropriate applications:

- High rate file transfer > 20 MB/s: Forward error correction does not work at these higher rates, so using a custom UDP-based congestion control works better
- Lossy real-time audio/video conference or chat: SiameseFEC has an infinite window, so using Cauchy Caterpillar is more appropriate: https://github.com/catid/CauchyCaterpillar
- High security applications: Tonk does not implement any PKI or encryption

### Message delivery:

+ Unordered unreliable message delivery
+ Unordered reliable message delivery
+ Multiple reliable in-order channels (6)
+ Multiple reliable in-order low-priority channels (6)
+ Breaks up large messages into pieces that fit into datagrams
+ Detects and rejects duplicate packets (incl. unreliable)
+ File transfer API for sending large buffers or files from disk (See tonk_file_transfer.h)

### Latency features:

+ 0-RTT connections: Server accepts data in first packet
+ Lower latency than TCP congestion control (10 milliseconds)
+ Enables file transfer coexisting with low latency applications
+ Forward Error Correction (FEC) to reduce median latency
+ Ordered reliable data is compressed with Zstd for speedier delivery

### Extras:

+ Jitter-robust time synchronization for mobile applications
+ NAT hole punching using UPnP for Peer2Peer connections
+ NAT hole punching using STUN + NATBLASTER (with rendezvous server)
+ Automatically opens up any needed ports in the desktop firewall
+ Detects and rejects data tampering on the wire
+ SYN-cookies enabled during connection floods to mitigate DoS attacks
+ Fast obfuscation (encryption without security guarantees)
+ Reduced idle traffic using ack-acks

### Not provided (Non-goals):

- Data security (Requirements are too application-specific)
- TCP fallback if UDP-based handshakes fail (Application can do this)
- Unreliable-but-ordered delivery (Can be done using Unreliable channel)
- MTU detection (Assumes 1488 byte frames or larger)


#### Credits

Tonk uses the Siamese library for its forward error correction (FEC),
    selective acknowledgements (SACK), and jitter buffer.
Tonk uses the Asio library for portable networking and strands.
Tonk uses the Cymric library for secure random number generation.
    Cymric uses BLAKE2b and Chacha.
Tonk uses the Zstd library (modified) for fast in-order packet compression.
Tonk uses the t1ha library for fast hashing.

Software by Christopher A. Taylor mrcatid@gmail.com

Please reach out if you need support or would like to collaborate on a project.
