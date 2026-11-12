namespace LEDspot.Models
{
    // JSON 저장 내용: 되돌리기에 필요한 설정 + 결과표
    internal class SavedSession
    {
        public RunSettings Settings { get; set; } = new RunSettings();
        public List<RoundRecord> Records { get; set; } = new List<RoundRecord>();
    }
}
