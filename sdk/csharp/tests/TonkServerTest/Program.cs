using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;
using TonkSDK;

namespace TonkServerTest
{
    class MyServerConnection : SDKConnection
    {
        int StatusLogCounter = 0;
        MyServer Server = null;

        OWDTimeStatistics OWDStats = new OWDTimeStatistics();
        int OWDStatsInterval = 0;

        UInt32 MBCounter = 0;

        public bool P2PRequested = false;

        public MyServerConnection(MyServer server)
        {
            Server = server;
        }

        public override void OnConnect()
        {
            TestTools.LogTonkStatus(this);

            Tonk.Status status = GetStatus();

            List<MyServerConnection> connections = Server.GetConnections();
            connections.ForEach(connection =>
            {
                Tonk.Status cs = connection.GetStatus();

                // Send other clients info about connecting client
                if (connection != this)
                {
                    Console.WriteLine("Broadcasting connection {0} about {1} connecting", cs.LocallyAssignedIdForRemoteHost,
                        status.LocallyAssignedIdForRemoteHost);

                    byte[] assignIdMsg = new byte[1 + 4];
                    unsafe
                    {
                        fixed(byte* ptr = assignIdMsg)
                        {
                            ptr[0] = Constants.ID_ConnectionAdded;
                            Tonk.WriteU32_LE(ptr + 1, status.LocallyAssignedIdForRemoteHost);
                        }
                    }
                    connection.Send(assignIdMsg);
                }

                // Send connecting client info about other clients
                byte[] userListIdMsg = new byte[1 + 4];
                unsafe
                {
                    fixed (byte* ptr = userListIdMsg)
                    {
                        ptr[0] = Constants.ID_ConnectionAdded;
                        Tonk.WriteU32_LE(ptr + 1, cs.LocallyAssignedIdForRemoteHost);
                    }
                }
                Send(userListIdMsg);
            });
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
            if (data[0] == Constants.ID_P2PConnectionStart)
            {
                P2PRequested = true;

                Tonk.Status statusBob = GetStatus();

                List<MyServerConnection> connections = Server.GetConnections();
                connections.ForEach(connection =>
                {
                    // If there is a second connection that also requested P2P:
                    if (connection != this && connection.P2PRequested)
                    {
                        Tonk.Status statusAlice = connection.GetStatus();

                        Console.WriteLine("Connecting Alice({0}) and Bob({1})",
                            statusAlice.LocallyAssignedIdForRemoteHost,
                            statusBob.LocallyAssignedIdForRemoteHost);

                        SDKResult result = P2PConnect(connection);
                        if (result.Failed())
                        {
                            Console.WriteLine("P2PConnect failed: {0}", result);
                        }
                    }
                });
                return;
            }
            if (data[0] == Constants.ID_ConnectionRebroadcast)
            {
                Tonk.Status status = GetStatus();

                string origMsg = Marshal.PtrToStringAnsi((IntPtr)data + 1);
                Console.WriteLine("Rebroadcast request from {0} = `{1}`", status.LocallyAssignedIdForRemoteHost, origMsg);

                byte[] broadcastMsg = new byte[3 + bytes];
                unsafe
                {
                    fixed (byte* ptr = broadcastMsg)
                    {
                        ptr[0] = Constants.ID_ConnectionRebroadcast;
                        Tonk.WriteU24_LE(ptr + 1, status.LocallyAssignedIdForRemoteHost);
                        for (int i = 1; i < bytes; ++i)
                            ptr[i + 3] = data[i];
                    }
                }

                List<MyServerConnection> connections = Server.GetConnections();
                connections.ForEach(connection =>
                {
                    // If there is a second connection that also requested P2P:
                    if (connection != this)
                    {
                        connection.Send(broadcastMsg, 1);
                    }
                });
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

            Console.WriteLine("Got ", bytes, " bytes of message data unexpected type ", (int)data[0]);
            Debug.Assert(false);
        }

        public override void OnTick(ulong nowUsec)
        {
            Tonk.Status status = GetStatus();

            if (++StatusLogCounter >= 100)
            {
                StatusLogCounter = 0;
                TestTools.LogTonkStatus(this);
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

                    unsafe
                    {
                        fixed (byte* ptr = data)
                        {
                            ptr[0] = 1;
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
            Console.WriteLine("OnClose({0})", reason);

            TestTools.LogTonkStatus(this);

            Server.Remove(this);
        }
    }

    class MyServer : SDKSocket
    {
        public bool Initialize()
        {
            // Set configuration
            Config.UDPListenPort = Constants.UDPServerPort;
            Config.MaximumClients = 10;

            SDKJsonResult result = Create();
            if (result.Failed())
            {
                Console.WriteLine("Unable to create socket: {0}", result);
                return result.Good();
            }

            return result.Good();
        }

        protected override SDKConnection OnIncomingConnection(SDKAddress address)
        {
            MyServerConnection connection = new MyServerConnection(this);
            lock (ClientListLock)
            {
                ClientList.Add(connection);
            }
            return connection;
        }

        // List of connections
        private Object ClientListLock = new Object();
        private List<MyServerConnection> ClientList = new List<MyServerConnection>();

        public List<MyServerConnection> GetConnections()
        {
            List<MyServerConnection> connections;
            lock (ClientListLock)
            {
                connections = new List<MyServerConnection>(ClientList);
            }
            return connections;
        }

        public void Remove(MyServerConnection connection)
        {
            lock (ClientListLock)
            {
                ClientList.Remove(connection);
            }
        }
    };

    class Program
    {
        static void RunServer()
        {
            MyServer server = new MyServer();

#if false
            // Test code: Verify that objects do not go out of scope too soon
            GC.Collect();
            GC.WaitForPendingFinalizers();
#endif

            if (server.Initialize())
            {
#if false
            // Test code: Verify that objects do not go out of scope too soon
            GC.Collect();
            GC.WaitForPendingFinalizers();
#endif

                Console.WriteLine("Press ENTER key to stop server");
                Console.ReadLine();
                Console.WriteLine("...Key press detected.  Stopping..");
            }

#if false
            // Test code: Verify that objects do not go out of scope too soon
            GC.Collect();
            GC.WaitForPendingFinalizers();
#endif

            server.Dispose();

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

            // Copy TonkDebug.dll in place of Tonk.dll so we use the debug version
#if DEBUG
            Tonk.ReplaceWithDebugDll();
#endif

            // Set up test tools
            TestTools.SetupTonkInternals();

            RunServer();

            Console.WriteLine("Press ENTER key to terminate");
            Console.ReadLine();
            Console.WriteLine("...Key press detected.  Terminating..");
        }
    }
}
