namespace LEDspot.Models
{
    // LED_WRITE, LED_RESP에 쓰는 색 번호
    internal enum LedColor : byte
    {
        Off = 0x00,
        Green = 0x01,
        Yellow = 0x02,
        Red = 0x03
    }
}
