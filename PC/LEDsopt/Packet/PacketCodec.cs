namespace LEDspot.Packet
{
    internal class PacketCodec
    {
        public static byte CalcCrc(byte[] data, int start, int len)
        {
            byte crc = 0;
            for (int i = start; i < start + len; i++)
            {
                crc ^= data[i];
                for (int j = 0; j < 8; j++)
                {
                    if ((crc & 0x80) != 0)
                    {
                        crc = (byte)(crc << 1 ^ 0x07);
                    }
                    else
                    {
                        crc = crc <<= 1;
                    }
                }
            }
            return crc;
        }
    }
}
