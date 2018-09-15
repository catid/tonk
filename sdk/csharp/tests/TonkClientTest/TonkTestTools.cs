using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using TonkSDK;

namespace TonkClientTest
{
    public class Constants
    {
        public const UInt16 UDPServerPort = 5060;

        public const string TONK_TEST_SERVER_IP = "localhost";

        public const byte ID_P2PConnectionStart = 2;
        public const byte ID_ConnectionAdded = 3;
        public const byte ID_ConnectionRemoved = 4;
        public const byte ID_ConnectionRebroadcast = 5;
        public const byte ID_PreConnectDataTest = 6;

        public const bool TONK_ENABLE_1MBPS_DATA = true;

        public const uint OWDStatsInterval = 200;
    }

    public class TestTools
    {
        public static void LogTonkStatus(SDKConnection connection)
        {
            Tonk.Status status = connection.GetStatus();
            Tonk.StatusEx statusEx = connection.GetStatusEx();

            StringBuilder sb = new StringBuilder();
            sb.Append("tonk_status: ");
            if (status.Flags.HasFlag(Tonk.Flags.Initiated))
                sb.Append("Initiated ");
            if (status.Flags.HasFlag(Tonk.Flags.TimeSync))
                sb.Append("TimeSync ");
            if (status.Flags.HasFlag(Tonk.Flags.NATMap_Local))
                sb.Append("NATMap_Local ");
            if (status.Flags.HasFlag(Tonk.Flags.NATMap_Remote))
                sb.Append("NATMap_Remote ");
            if (status.Flags.HasFlag(Tonk.Flags.Connecting))
                sb.Append("Connecting ");
            if (status.Flags.HasFlag(Tonk.Flags.Connected))
                sb.Append("Connected ");
            if (status.Flags.HasFlag(Tonk.Flags.Disconnected))
                sb.Append("Disconnected ");
            sb.AppendFormat(" RemoteId={0}", status.LocallyAssignedIdForRemoteHost);
            sb.AppendFormat(" AppBPS={0}", status.AppBPS);
            sb.AppendFormat(" ReliableQueueMsec={0}", status.ReliableQueueMsec);
            sb.AppendFormat(" LowPriQueueMsec={0}", status.LowPriQueueMsec);

            sb.AppendFormat(" ConnectionKey={0}", statusEx.ConnectionKey.ToString("X4"));
            sb.AppendFormat(" Local={0}", statusEx.Local.ToString());
            sb.AppendFormat(" Remote={0}", statusEx.Remote.ToString());
            sb.AppendFormat(" LocalNATPort={0}", statusEx.LocalNATMapExternalPort);
            sb.AppendFormat(" RemoteNATPort={0}", statusEx.RemoteNATMapExternalPort);
            sb.AppendFormat(" LocalId={0}", statusEx.RemoteAssignedIdForLocalHost);
            sb.AppendFormat(" PeerSeenBPS={0}", statusEx.PeerSeenBPS);
            sb.AppendFormat(" PeerSeenLossRate={0}", statusEx.PeerSeenLossRate);
            sb.AppendFormat(" PeerTripUsec={0}", statusEx.PeerTripUsec);
            sb.AppendFormat(" IncomingBPS={0}", statusEx.IncomingBPS);
            sb.AppendFormat(" PLR={0}", statusEx.IncomingLossRate);
            sb.AppendFormat(" TripUsec={0}", statusEx.TripUsec);

            Console.WriteLine(sb);
        }

        public const uint PreConnectDataBytes = 10;
        public const uint PreConnectChannel = Tonk.Channel.Reliable0 + 5;
        public static readonly byte[] PreConnectData = new byte[] {
            Constants.ID_PreConnectDataTest,
            9, 8, 7, 6, 5, 4, 3, 2, 1
        };

        public static bool SendPreconnectData(SDKConnection connection)
        {
            string Hello = "Pre-connect message";

            byte[] hello = new byte[1 + Hello.Length + 1];
            hello[0] = Constants.ID_ConnectionRebroadcast;

            for (int i = 0; i < Hello.Length; ++i)
                hello[i + 1] = (byte)Hello[i];
            hello[1 + Hello.Length] = 0;

            SDKResult result = connection.Send(hello);

            if (result.Failed())
            {
                Console.WriteLine("tonk_send 1 failed: ", result.ToString());
                return false;
            }

            result = connection.Send(PreConnectData, PreConnectChannel);

            if (result.Failed())
            {
                Console.WriteLine("tonk_send 2 failed: ", result.ToString());
                return false;
            }

            Console.WriteLine("Sent preconnect data");

            connection.Flush();
            return true;
        }

        public static unsafe bool ValidatePreconnectData(UInt32 channel, byte* data, UInt32 bytes)
        {
            if (bytes != PreConnectDataBytes || channel != PreConnectChannel)
                return false;

            for (uint i = 0; i < PreConnectDataBytes; ++i)
                if (data[i] != PreConnectData[i])
                    return false;

            return true;
        }
    }

    public class OWDTimeStatistics
    {
        public const int kMaxSamples = 1000;
        public UInt64[] Samples = new UInt64[kMaxSamples];

        public int SampleCount = 0;
        public int SampleIndex = 0;

        public void Clear()
        {
            SampleCount = 0;
            SampleIndex = 0;
        }
        public void AddSample(UInt64 owdUsec)
        {
            if (SampleCount < kMaxSamples)
                ++SampleCount;

            Samples[SampleIndex] = owdUsec;

            // Pick next sample index
            if (++SampleIndex >= kMaxSamples)
                SampleIndex = 0;
        }
        public void PrintStatistics()
        {
            Array.Sort(Samples, 0, SampleCount);

            UInt64 percentile1 = 0;
            if (SampleCount > 200)
            {
                int goalOffset = (int)(0.99 * SampleCount);
                percentile1 = Samples[goalOffset];
            }

            UInt64 percentile5 = 0;
            if (SampleCount > 100)
            {
                int goalOffset = (int)(0.95 * SampleCount);
                percentile5 = Samples[goalOffset];
            }

            UInt64 percentile25 = 0;
            if (SampleCount > 4)
            {
                int goalOffset = (int)(0.75 * SampleCount);
                percentile25 = Samples[goalOffset];
            }

            UInt64 percentile50 = 0;
            if (SampleCount > 4)
            {
                int goalOffset = (int)(0.5 * SampleCount);
                percentile50 = Samples[goalOffset];
            }

            UInt64 percentile75 = 0;
            if (SampleCount > 8)
            {
                int goalOffset = (int)(0.25 * SampleCount);
                percentile75 = Samples[goalOffset];
            }

            UInt64 percentile95 = 0;
            if (SampleCount > 40)
            {
                int goalOffset = (int)(0.05 * SampleCount);
                percentile95 = Samples[goalOffset];
            }

            UInt64 percentile99 = 0;
            if (SampleCount > 200)
            {
                int goalOffset = (int)(0.01 * SampleCount);
                percentile99 = Samples[goalOffset];
            }

            Console.WriteLine("One-way  1% percentile latency = {0} msec", percentile1 / 1000.0f);
            Console.WriteLine("One-way  5% percentile latency = {0} msec", percentile5 / 1000.0f);
            Console.WriteLine("One-way 25% percentile latency = {0} msec", percentile25 / 1000.0f);
            Console.WriteLine("One-way 50% percentile latency = {0} msec (median)", percentile50 / 1000.0f);
            Console.WriteLine("One-way 75% percentile latency = {0} msec", percentile75 / 1000.0f);
            Console.WriteLine("One-way 95% percentile latency = {0} msec", percentile95 / 1000.0f);
            Console.WriteLine("One-way 99% percentile latency = {0} msec", percentile99 / 1000.0f);
        }
    }
}
