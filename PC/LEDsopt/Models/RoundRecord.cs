namespace LEDspot.Models
{
    // 결과표 한 줄 = 한 회차. DataGrid 열: 회차, 방식, 랜덤 숫자, 색, 성공 여부, 시간
    internal class RoundRecord
    {
        public int Round { get; set; }            // 회차
        public string Mode { get; set; } = "";    // 방식 (A/B)
        public string Number { get; set; } = "";  // 랜덤 숫자
        public string Color { get; set; } = "";   // 색
        public bool Success { get; set; }         // 성공 여부
        public string Time { get; set; } = "";    // 시간
    }
}
