using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;
using TonkSDK;

namespace TonkClientTest
{
    class MyClientConnection : SDKConnection
    {
        int StatusLogCounter = 0;
        MyClient Client = null;

        OWDTimeStatistics OWDStats = new OWDTimeStatistics();
        int OWDStatsInterval = 0;

        UInt32 MBCounter = 0;

        public MyClientConnection(MyClient client)
        {
            Client = client;
        }

        public override void OnConnect()
        {
            TestTools.LogTonkStatus(this);
        }

        public override unsafe void OnData(uint channel, byte* data, uint bytes)
        {
            if (bytes <= 0)
                return;

            if (data[0] == 1)
            {
                UInt16 ts16 = Tonk.ReadU16_LE(data + 1);
                UInt32 ts23 = Tonk.ReadU24_LE(data + 1 + 2);
                UInt64 lo16 = FromLocalTime16(ts16);
                UInt64 lo23 = FromLocalTime23(ts23);
                UInt32 magic = Tonk.ReadU24_LE(data + 1 + 2 + 3);
                Debug.Assert(magic == 0x123456);
                UInt32 packetIndex = Tonk.ReadU32_LE(data + 1 + 2 + 3 + 3);

                UInt64 nowUsec = Tonk.tonk_time();
                UInt64 owdUsec = nowUsec - lo23;
                OWDStats.AddSample(owdUsec);
                if (++OWDStatsInterval >= Constants.OWDStatsInterval)
                {
                    OWDStatsInterval = 0;
                    Console.WriteLine("DATA RESULT: Transmission time (acc=16) = {0}, (acc=23) = {1} # {2}", (int)(nowUsec - lo16), (int)(nowUsec - lo23), packetIndex);
                    OWDStats.PrintStatistics();
                }

                return;
            }
            if (data[0] == Constants.ID_ConnectionAdded)
            {
                UInt32 id = Tonk.ReadU32_LE(data + 1);
                Console.WriteLine("Server reported new connection: ID = {0}", id);
                return;
            }
            if (data[0] == Constants.ID_ConnectionRemoved)
            {
                UInt32 id = Tonk.ReadU32_LE(data + 1);
                Console.WriteLine("Server reported removed connection: ID = {0}", id);
                return;
            }
            if (data[0] == Constants.ID_ConnectionRebroadcast)
            {
                UInt32 id = Tonk.ReadU24_LE(data + 1);
                string msg = Marshal.PtrToStringAnsi((IntPtr)data + 1 + 3);
                Console.WriteLine("Server rebroadcast from ID = {0} : `{1}`", id, msg);
                return;
            }
            if (data[0] == Constants.ID_PreConnectDataTest)
            {
                Console.WriteLine("Got pre-connect data test");

                bool validated = TestTools.ValidatePreconnectData(channel, data, bytes);
                if (!validated)
                {
                    Console.WriteLine("ERROR: Preconnect data was invalid!");
                    Debug.Assert(false);
                }
                return;
            }

            Console.WriteLine("MyServerConnection: Got ", bytes, " bytes of message data unexpected type ", (int)data[0]);
            Debug.Assert(false);
        }

        public override void OnTick(ulong nowUsec)
        {
            Tonk.Status status = GetStatus();

            if (++StatusLogCounter >= 100)
            {
                StatusLogCounter = 0;
                TestTools.LogTonkStatus(this);

                if (status.Flags.HasFlag(Tonk.Flags.TimeSync))
                {
                    if (!Client.StartedP2PConnection)
                    {
                        // Tell server we want to connect to another peer
                        byte[] data = new byte[1];
                        data[0] = Constants.ID_P2PConnectionStart;
                        Send(data);
                    }
                }
            }

            if (Constants.TONK_ENABLE_1MBPS_DATA)
            {
                if (status.Flags.HasFlag(Tonk.Flags.TimeSync))
                {
                    byte[] data = new byte[1 + 2 + 3 + 1300 - 4];

                    for (int i = 0; i < data.Length; ++i)
                        data[i] = (byte)i;

                    UInt16 ts16 = ToRemoteTime16(nowUsec);
                    UInt32 ts23 = ToRemoteTime23(nowUsec);

                    data[0] = 1;
                    unsafe
                    {
                        fixed (byte* ptr = data)
                        {
                            Tonk.WriteU16_LE(ptr + 1, ts16);
                            Tonk.WriteU24_LE(ptr + 1 + 2, ts23);
                            Tonk.WriteU24_LE(ptr + 1 + 2 + 3, 0x123456);
                            Tonk.WriteU32_LE(ptr + 1 + 2 + 3 + 3, MBCounter++);
                        }
                    }

                    Send(data);
                }
            }

            // Test destroying the object before app shuts down.
            // Ensure that GC collects the SDKConnection object shortly after this.
            //Close();
        }

        public override void OnClose(SDKJsonResult reason)
        {
            Console.WriteLine("MyServerConnection: OnClose({0})", reason.ToString());

            TestTools.LogTonkStatus(this);
        }
    }

    class MyP2PClientConnection : SDKConnection
    {
        int StatusLogCounter = 0;

        OWDTimeStatistics OWDStats = new OWDTimeStatistics();
        int OWDStatsInterval = 0;

        UInt32 MBCounter = 0;

        public override void OnConnect()
        {
            TestTools.LogTonkStatus(this);
        }

        public override unsafe void OnData(uint channel, byte* data, uint bytes)
        {
            if (bytes <= 0)
                return;

            if (data[0] == 1)
            {
                UInt16 ts16 = Tonk.ReadU16_LE(data + 1);
                UInt32 ts23 = Tonk.ReadU24_LE(data + 1 + 2);
                UInt64 lo16 = FromLocalTime16(ts16);
                UInt64 lo23 = FromLocalTime23(ts23);
                UInt32 magic = Tonk.ReadU24_LE(data + 1 + 2 + 3);
                Debug.Assert(magic == 0x123456);
                UInt32 packetIndex = Tonk.ReadU32_LE(data + 1 + 2 + 3 + 3);

                UInt64 nowUsec = Tonk.tonk_time();
                UInt64 owdUsec = nowUsec - lo23;
                OWDStats.AddSample(owdUsec);
                if (++OWDStatsInterval >= Constants.OWDStatsInterval)
                {
                    OWDStatsInterval = 0;
                    Console.WriteLine("DATA RESULT: Transmission time (acc=16) = {0}, (acc=23) = {1} # {2}", (int)(nowUsec - lo16), (int)(nowUsec - lo23), packetIndex);
                    OWDStats.PrintStatistics();
                }

                return;
            }

            Console.WriteLine("MyP2PConnection: Got ", bytes, " bytes of message data unexpected type ", (int)data[0]);
            Debug.Assert(false);
        }

        public override void OnTick(ulong nowUsec)
        {
            if (++StatusLogCounter >= 100)
            {
                StatusLogCounter = 0;
                TestTools.LogTonkStatus(this);
            }

            if (Constants.TONK_ENABLE_1MBPS_DATA)
            {
                Tonk.Status status = GetStatus();

                if (status.Flags.HasFlag(Tonk.Flags.TimeSync))
                {
                    byte[] data = new byte[1 + 2 + 3 + 1300 - 4];

                    for (int i = 0; i < data.Length; ++i)
                        data[i] = (byte)i;

                    UInt16 ts16 = ToRemoteTime16(nowUsec);
                    UInt32 ts23 = ToRemoteTime23(nowUsec);

                    data[0] = 1;
                    unsafe
                    {
                        fixed (byte* ptr = data)
                        {
                            Tonk.WriteU16_LE(ptr + 1, ts16);
                            Tonk.WriteU24_LE(ptr + 1 + 2, ts23);
                            Tonk.WriteU24_LE(ptr + 1 + 2 + 3, 0x123456);
                            Tonk.WriteU32_LE(ptr + 1 + 2 + 3 + 3, MBCounter++);
                        }
                    }

                    Send(data);
                }
            }
        }

        public override void OnClose(SDKJsonResult reason)
        {
            Console.WriteLine("MyP2PConnection: OnClose({0})", reason.ToString());

            TestTools.LogTonkStatus(this);
        }
    }

    class MyClient : SDKSocket
    {
        public bool Initialize()
        {
            // Set configuration
            Config.UDPListenPort = 0;
            Config.MaximumClients = 10;

            SDKJsonResult result = Create();
            if (result.Failed())
            {
                Console.WriteLine("Unable to create socket: {0}", result.ToString());
                return result.Good();
            }

            MyClientConnection conn = new MyClientConnection(this);

            result = Connect(
                conn,
                Constants.TONK_TEST_SERVER_IP,
                Constants.UDPServerPort);

            if (result.Failed())
            {
                Console.Write("Unable to connect: {0}", result.ToString());
            }
            else
            {
                TestTools.SendPreconnectData(conn);
            }

            return result.Good();
        }

        protected override SDKConnection OnP2PConnectionStart(SDKAddress address)
        {
            StartedP2PConnection = true;
            return new MyP2PClientConnection();
        }

        public bool StartedP2PConnection = false;
    };

    class Program
    {
        static void RunClient()
        {
            MyClient client = new MyClient();

#if false
            // Test code: Verify that objects do not go out of scope too soon
            GC.Collect();
            GC.WaitForPendingFinalizers();
#endif

            if (client.Initialize())
            {
#if false
            // Test code: Verify that objects do not go out of scope too soon
            GC.Collect();
            GC.WaitForPendingFinalizers();
#endif

                Console.WriteLine("Press ENTER key to stop client");
                Console.ReadLine();
                Console.WriteLine("...Key press detected.  Stopping..");
            }

#if false
            // Test code: Verify that objects do not go out of scope too soon
            GC.Collect();
            GC.WaitForPendingFinalizers();
#endif

            client.Dispose(); // Shutdown the Tonk object

#if false
            // Test code: Verify that objects do not go out of scope too soon
            GC.Collect();
            GC.WaitForPendingFinalizers();
#endif
        }

        static void Main(string[] args)
        {
            // Choose linkage where we expect the Tonk.dll to be found under x86/ or x86_64/ based on platform
            Tonk.OverrideDLLDirectory();

            // Copy tonk_debug.dll in place of tonk.dll so we use the debug version
#if DEBUG
            Tonk.ReplaceWithDebugDll();
#endif

            RunClient();

            Console.WriteLine("Press ENTER key to terminate");
            Console.ReadLine();
            Console.WriteLine("...Key press detected.  Terminating..");
        }
    }
}
