using LEDspot.Packet;
using System.Collections.Concurrent;
using System.IO.Ports;

namespace LEDspot.Communication
{
    internal class Serial
    {
        public Task? rxTask, procTask;
        public bool collecting = false;
        public bool running = false;
        public SerialPort serialPort;
        public string lastError = "";
        private readonly object sendLock = new object();
        public List<byte> rxBuf = new List<byte>();
        public ConcurrentQueue<byte[]> pktQueue = new ConcurrentQueue<byte[]>();

        public event Action<byte[]>? PacketReceived;   // CRC, 끝 바이트가 맞는 패킷
        public event Action<byte[]>? CrcFailed;        // CRC가 틀린 패킷 (NAK는 여기서 이미 보냄)

        public Serial()
        {
            serialPort = new SerialPort();
        }
        public bool Connect(string portName, int baudRate)
        {
            try
            {
                Disconnect();

                serialPort.PortName = portName;
                serialPort.BaudRate = baudRate;
                serialPort.Open();
                return true;
            }
            catch (Exception ex)
            {
                lastError = ex.Message;
                Disconnect();
                return false;
            }
        }
        public void Disconnect()
        {
            // 수신 스레드를 먼저 멈춘 뒤 포트를 닫는다 (닫힌 포트를 읽다가 예외가 나지 않도록)
            running = false;
            try
            {
                rxTask?.Wait(200);
                procTask?.Wait(200);
            }
            catch (AggregateException)
            {
                // 수신 스레드가 예외로 끝난 경우 (예: 케이블 분리) 무시하고 닫는다
            }

            if (serialPort != null)
            {
                if (serialPort.IsOpen)
                {
                    serialPort.Close();
                }
            }
        }
        public void SendPacket(byte[] data, int cmd, int len)
        {
            byte[] pkt = new byte[5 + data.Length];

            lock (sendLock)
            {

                pkt[0] = Protocol.STX;
                pkt[1] = (byte)cmd;
                pkt[2] = (byte)len;
                for (int i = 0; i < data.Length; i++)
                {
                    pkt[3 + i] = data[i];
                }
                pkt[3 + len] = PacketCodec.CalcCrc(pkt, 1, 2 + len);
                pkt[4 + len] = Protocol.ETX;
                serialPort.Write(pkt, 0, pkt.Length);
            }
        }
        public void StartThreads()
        {
            collecting = false;
            rxBuf.Clear();
            pktQueue.Clear();
            running = true;
            rxTask = Task.Run(ReceiveLoop);
            procTask = Task.Run(ProcessLoop);

        }
        private void ReceiveLoop()
        {
            while (running)
            {
                if (serialPort.BytesToRead == 0)
                {
                    Thread.Sleep(5);
                    continue;
                }
                byte b = (byte)serialPort.ReadByte();

                if (!collecting)
                {
                    if (b == Protocol.STX)
                    {
                        collecting = true;
                        rxBuf.Clear();
                        rxBuf.Add(b);
                    }
                    continue;
                }

                rxBuf.Add(b);

                // 끝 바이트를 찾지 않고 LEN으로 길이를 계산해 자른다 (숫자 2, 3이 STX/ETX와 같을 수 있음)
                if (rxBuf.Count >= 5 && rxBuf.Count == rxBuf[2] + 5)//len 데이터 길이 + STX, CMD, LEN, CRC, ETX
                {
                    byte[] pkt = rxBuf.ToArray();
                    pktQueue.Enqueue(pkt);
                    collecting = false;
                    rxBuf.Clear();
                }
                else if (rxBuf.Count >= 32)
                {
                    collecting = false;
                    rxBuf.Clear();
                }


            }

        }

        void ProcessLoop()
        {
            while (running || !pktQueue.IsEmpty)
            {
                if (pktQueue.TryDequeue(out byte[]? pkt))
                {
                    int len = pkt[2];
                    byte etx = pkt[4 + len];
                    byte receivedCrc = pkt[3 + len];
                    byte myCrc = PacketCodec.CalcCrc(pkt, 1, 2 + len);

                    bool endOk = etx == Protocol.ETX;
                    bool crcOk = receivedCrc == myCrc;

                    if (endOk && crcOk)
                    {
                        PacketReceived?.Invoke(pkt);
                    }
                    else if (endOk && !crcOk)
                    {
                        // CRC가 틀리면 NAK (DATA = 받은 CMD). ACK/NAK 자체에는 응답하지 않는다
                        if (pkt[1] != Protocol.ACK && pkt[1] != Protocol.NAK)
                        {
                            try
                            {
                                SendPacket(new byte[] { pkt[1] }, Protocol.NAK, 1);
                            }
                            catch (Exception ex)
                            {
                                lastError = ex.Message;
                            }
                        }
                        CrcFailed?.Invoke(pkt);
                    }
                }
                else Thread.Sleep(5);
            }
        }
    }
}
