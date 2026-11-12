namespace LEDspot.Packet
{
    internal static class Protocol
    {
        // 프레임 바이트
        public const byte STX = 0x02;
        public const byte ETX = 0x03;

        // 명령 (CMD)
        public const byte LED_WRITE = 0x01;   // PC → MCU, 색 번호
        public const byte LED_MASK = 0x02;    // PC → MCU, 비트 마스크 (bit0 초록, bit1 노랑, bit2 빨강)
        public const byte ACK = 0x06;         // MCU → PC, 받은 CMD
        public const byte NAK = 0x15;         // MCU → PC, 받은 CMD
        public const byte LED_RESP = 0x82;    // MCU → PC, 현재 색 번호
        public const byte RAND_SEND = 0x20;   // MCU → PC, 자리마다 1바이트 (0~9)
        public const byte RAND_REQ = 0x21;    // PC → MCU, 자리마다 1바이트 (0~9)
        public const byte RAND_NEXT = 0x22;   // PC → MCU, 없음 (LEN 0)
    }
}
