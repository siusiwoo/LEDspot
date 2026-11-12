using LEDspot.Communication;
using LEDspot.Models;
using LEDspot.Packet;
using System.Collections.ObjectModel;
using System.IO;
using System.IO.Ports;
using System.Text;
using System.Text.Json;
using System.Windows;
using System.Windows.Media;
namespace LEDspot
{
    /// <summary>
    /// Interaction logic for MainWindow.xaml
    /// </summary>
    public partial class MainWindow : Window
    {
        // ===== 미정 항목: 제안값을 상수로 둔다. 바꿀 때는 여기만 고치면 된다 =====
        const int BaudRate = 115200;                          // LED02와 같은 값
        const int CountdownStepMs = 1000;                     // 카운트다운 단계별 시간 [미정, 제안 1초]
        const int ResponseTimeoutMs = 1000;                   // 응답 대기 시간 [미정, 제안 1초]
        const int LedOffGapMs = 100;                          // 색을 바꿀 때 꺼져 있는 시간 [미정, 제안 100ms, 보드와 같게]
        static readonly bool RecordFailOnNak = false;         // A 방식: CRC 오류(NAK) 시 실패 행 기록 [미정, 제안: 기록 안 하고 종료]
        static readonly bool BSuccessNeedsColorMatch = true;  // B 방식: LED_RESP 색 = PC 예상 색일 때만 성공 [미정, 제안: 예]

        string jsonPath = "settings.json";
        SavedSession savedSession = new SavedSession();

        Serial serial = new Serial();
        ObservableCollection<RoundRecord> records = new ObservableCollection<RoundRecord>();
        Random rng = new Random();

        bool running = false;     // 실행 중 여부 (A/B 공통)

        // 메타데이터 표시용
        string modeText = "-";
        string roundText = "-";
        string crcText = "-";
        string noticeText = "";

        // 응답 대기 (모두 UI 스레드에서만 다룬다)
        TaskCompletionSource<byte[]?>? randWaiter;   // A: RAND_NEXT 뒤 RAND_SEND (null = CRC 오류)
        TaskCompletionSource<bool>? ackWaiter;        // ACK이면 true, NAK이면 false
        byte ackWaitCmd;                              // 어떤 CMD에 대한 ACK를 기다리는지
        TaskCompletionSource<byte>? respWaiter;       // B: LED_RESP 색 번호

        public MainWindow()
        {
            InitializeComponent();
            ResultGrid.ItemsSource = records;
            SetMask(0);

            serial.PacketReceived += HandlePacket;
            serial.CrcFailed += HandleCrcFailed;
            Closing += (s, e) => serial.Disconnect();

            RefreshPorts();
            if (PortBox.Items.Contains("COM3"))
            {
                PortBox.SelectedItem = "COM3";
                Connect("COM3");
            }
            UpdateMeta();
        }

        // ================= 포트 연결 =================

        void RefreshPorts()
        {
            object? selected = PortBox.SelectedItem;
            PortBox.ItemsSource = SerialPort.GetPortNames().OrderBy(p => p).ToList();
            if (selected != null && PortBox.Items.Contains(selected))
            {
                PortBox.SelectedItem = selected;
            }
        }

        private void PortBox_DropDownOpened(object sender, EventArgs e)
        {
            RefreshPorts();
        }

        private void ConnectButton_Click(object sender, RoutedEventArgs e)
        {
            if (serial.serialPort.IsOpen)
            {
                serial.Disconnect();
                UpdateMeta();
                return;
            }
            if (PortBox.SelectedItem is not string port)
            {
                MessageBox.Show("COM 포트를 선택하세요.");
                return;
            }
            Connect(port);
        }

        void Connect(string port)
        {
            if (serial.Connect(port, BaudRate))
            {
                serial.StartThreads();
            }
            else
            {
                MessageBox.Show($"연결 중 오류가 발생하였습니다: {serial.lastError}");
            }
            UpdateMeta();
        }

        // ================= 수신 처리 =================

        // 수신 스레드에서 호출되므로 UI 스레드로 넘긴다
        public void HandlePacket(byte[] data)
        {
            Dispatcher.BeginInvoke(() => HandlePacketOnUi(data));
        }

        void HandlePacketOnUi(byte[] data)
        {
            crcText = "OK";
            byte len = data[2];
            switch (data[1])
            {
                case Protocol.ACK:
                case Protocol.NAK:
                    // DATA = 받은 CMD. 기다리던 CMD에 대한 응답이면 결과를 넘긴다
                    if (ackWaiter != null && len >= 1 && data[3] == ackWaitCmd)
                    {
                        ackWaiter.TrySetResult(data[1] == Protocol.ACK);
                    }
                    break;

                case Protocol.RAND_SEND:
                    byte[] digits = ReadData(data);
                    if (randWaiter != null)
                    {
                        randWaiter.TrySetResult(digits);   // A 방식: RAND_NEXT에 대한 응답
                    }
                    else if (!running)
                    {
                        _ = RunModeA(digits);              // 보드 USER 버튼으로 A 방식 시작
                    }
                    // 실행 중 그 밖의 RAND_SEND는 무시
                    break;

                case Protocol.LED_RESP:
                    if (respWaiter != null && len >= 1)
                    {
                        respWaiter.TrySetResult(data[3]);  // B 방식: MCU가 알려준 현재 색
                    }
                    break;
            }
            UpdateMeta();
        }

        // CRC가 틀린 패킷 (Serial에서 이미 NAK를 보냄)
        void HandleCrcFailed(byte[] data)
        {
            Dispatcher.BeginInvoke(() =>
            {
                crcText = $"NG (CMD 0x{data[1]:X2})";
                if (data[1] == Protocol.RAND_SEND && randWaiter != null)
                {
                    randWaiter.TrySetResult(null);   // A 방식 실행을 끝내도록 알림
                }
                UpdateMeta();
            });
        }

        // ================= A 방식: 보드 USER 버튼으로 시작 =================

        async Task RunModeA(byte[] firstDigits)
        {
            if (!TryReadInputs(out int repeat, out int interval))
            {
                return;   // 입력이 틀리면 실행하지 않음
            }

            BeginRun("A", repeat);
            try
            {
                Send(Protocol.ACK, new byte[] { Protocol.RAND_SEND });   // CRC가 맞았다는 응답
                await Countdown();                                        // 첫 회차 전에만

                byte[] digits = firstDigits;
                for (int round = 1; round <= repeat; round++)
                {
                    if (round > 1)
                    {
                        await Task.Delay(interval);

                        // 응답을 놓치지 않도록 대기 객체를 먼저 만든 뒤 RAND_NEXT를 보낸다
                        var waiter = new TaskCompletionSource<byte[]?>(TaskCreationOptions.RunContinuationsAsynchronously);
                        randWaiter = waiter;
                        Send(Protocol.RAND_NEXT, Array.Empty<byte>());
                        Task done = await Task.WhenAny(waiter.Task, Task.Delay(ResponseTimeoutMs));
                        randWaiter = null;

                        if (done != waiter.Task)
                        {
                            noticeText = $"{round}회차: RAND_SEND 응답 없음 → 실행 종료";
                            break;
                        }
                        byte[]? next = waiter.Task.Result;
                        if (next == null)
                        {
                            noticeText = $"{round}회차: CRC 오류 → NAK 보내고 실행 종료";
                            if (RecordFailOnNak)
                            {
                                AddRecord(round, "A", Array.Empty<byte>(), LedColor.Off, false);
                            }
                            break;
                        }
                        digits = next;
                        Send(Protocol.ACK, new byte[] { Protocol.RAND_SEND });
                    }

                    roundText = $"{round} / {repeat}";
                    UpdateMeta();

                    // 자리 수로 색을 판정해 LED_WRITE를 보내고 ACK를 기다린다
                    LedColor color = Judge(digits);
                    bool ack = await SendAndWaitAck(Protocol.LED_WRITE, new byte[] { (byte)color });
                    await ShowColor(color);
                    if (!ack)
                    {
                        noticeText = $"{round}회차: LED_WRITE ACK 없음";
                    }
                    AddRecord(round, "A", digits, color, ack);
                }
            }
            finally
            {
                EndRun();
            }
        }

        // ================= B 방식: PC 시작 버튼으로 시작 =================

        private async void StartButton_Click(object sender, RoutedEventArgs e)
        {
            if (running) return;
            if (!TryReadInputs(out int repeat, out int interval)) return;
            if (!serial.serialPort.IsOpen)
            {
                MessageBox.Show("보드가 연결되어 있지 않습니다.");
                return;
            }

            BeginRun("B", repeat);
            try
            {
                await Countdown();

                for (int round = 1; round <= repeat; round++)
                {
                    if (round > 1)
                    {
                        await Task.Delay(interval);
                    }
                    roundText = $"{round} / {repeat}";
                    UpdateMeta();

                    // PC가 숫자를 만들어 RAND_REQ로 보낸다
                    byte[] digits = RandomDigits();
                    LedColor expected = Judge(digits);

                    // LED_RESP를 놓치지 않도록 대기 객체를 먼저 만든다
                    var resp = new TaskCompletionSource<byte>(TaskCreationOptions.RunContinuationsAsynchronously);
                    respWaiter = resp;
                    bool ack = await SendAndWaitAck(Protocol.RAND_REQ, digits);
                    Task done = await Task.WhenAny(resp.Task, Task.Delay(ResponseTimeoutMs));
                    respWaiter = null;

                    bool gotResp = done == resp.Task;
                    LedColor shown = gotResp ? (LedColor)resp.Task.Result : expected;   // 보드가 알려준 색을 화면에 표시
                    bool ok = ack && gotResp && (!BSuccessNeedsColorMatch || shown == expected);

                    await ShowColor(shown);
                    if (!ack) noticeText = $"{round}회차: RAND_REQ ACK 없음";
                    else if (!gotResp) noticeText = $"{round}회차: LED_RESP 없음";
                    else if (shown != expected) noticeText = $"{round}회차: 색 불일치 (PC {ColorName(expected)}, 보드 {ColorName(shown)})";

                    AddRecord(round, "B", digits, shown, ok);
                }
            }
            finally
            {
                EndRun();
            }
        }

        // ================= 공통 =================

        // 카운트다운: LED_MASK 0x07(3) → 0x03(2) → 0x01(1) → 0x00(끝). 결과표/저장에는 기록하지 않는다
        async Task Countdown()
        {
            byte[] masks = { 0x07, 0x03, 0x01, 0x00 };
            foreach (byte mask in masks)
            {
                SetMask(mask);
                Task<bool> ackTask = SendAndWaitAck(Protocol.LED_MASK, new byte[] { mask });
                await Task.WhenAll(ackTask, Task.Delay(CountdownStepMs));
                if (!ackTask.Result)
                {
                    noticeText = $"카운트다운 ACK 없음 (mask 0x{mask:X2})";   // 메타데이터에만 표시
                    UpdateMeta();
                }
            }
        }

        // 보내고 해당 CMD의 ACK를 기다린다 (Thread.Sleep 대신 await + 타임아웃)
        async Task<bool> SendAndWaitAck(byte cmd, byte[] data)
        {
            var tcs = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
            ackWaiter = tcs;
            ackWaitCmd = cmd;
            if (!Send(cmd, data))
            {
                ackWaiter = null;
                return false;
            }
            Task done = await Task.WhenAny(tcs.Task, Task.Delay(ResponseTimeoutMs));
            if (ackWaiter == tcs)
            {
                ackWaiter = null;
            }
            return done == tcs.Task && tcs.Task.Result;
        }

        bool Send(byte cmd, byte[] data)
        {
            if (!serial.serialPort.IsOpen)
            {
                noticeText = "포트가 연결되어 있지 않습니다.";
                UpdateMeta();
                return false;
            }
            try
            {
                serial.SendPacket(data, cmd, data.Length);
                return true;
            }
            catch (Exception ex)
            {
                noticeText = $"송신 실패: {ex.Message}";
                UpdateMeta();
                return false;
            }
        }

        // 횟수: 1 이상의 정수, 간격: 0보다 큰 정수(ms)
        bool TryReadInputs(out int repeat, out int interval)
        {
            interval = 0;
            if (!int.TryParse(RepeatBox.Text.Trim(), out repeat) || repeat < 1)
            {
                MessageBox.Show("반복 횟수는 1 이상의 정수로 입력하세요.");
                return false;
            }
            if (!int.TryParse(IntervalBox.Text.Trim(), out interval) || interval <= 0)
            {
                MessageBox.Show("반복 간격은 0보다 큰 정수(ms)로 입력하세요.");
                return false;
            }
            return true;
        }

        // 실행 시작: 결과표 초기화, 버튼 비활성화
        void BeginRun(string mode, int repeat)
        {
            running = true;
            records.Clear();
            modeText = mode;
            roundText = $"0 / {repeat}";
            noticeText = "";
            SetButtons(false);
            UpdateMeta();
        }

        // 실행 종료: 대기 정리, 버튼 다시 활성화
        void EndRun()
        {
            running = false;
            randWaiter = null;
            ackWaiter = null;
            respWaiter = null;
            SetButtons(true);
            UpdateMeta();
        }

        void SetButtons(bool enabled)
        {
            StartButton.IsEnabled = enabled;
            SaveButton.IsEnabled = enabled;
            RestoreButton.IsEnabled = enabled;
            ConnectButton.IsEnabled = enabled;
            PortBox.IsEnabled = enabled;
        }

        void UpdateMeta()
        {
            bool open = serial.serialPort.IsOpen;
            string portText = open ? $"{serial.serialPort.PortName} 연결됨" : "연결 안 됨";
            ConnectButton.Content = open ? "연결 끊기" : "연결";
            MetaText.Text = $"방식: {modeText}    회차: {roundText}\nCRC: {crcText}    포트: {portText}\n{noticeText}";
        }

        void AddRecord(int round, string mode, byte[] digits, LedColor color, bool success)
        {
            records.Add(new RoundRecord
            {
                Round = round,
                Mode = mode,
                Number = string.Concat(digits.Select(d => d.ToString())),
                Color = ColorName(color),
                Success = success,
                Time = DateTime.Now.ToString("HH:mm:ss")
            });
            ResultGrid.ScrollIntoView(records[^1]);
        }

        // DATA 부분 복사 (data[2] = LEN, data[3]부터 DATA)
        static byte[] ReadData(byte[] data)
        {
            int len = data[2];
            byte[] result = new byte[len];
            Array.Copy(data, 3, result, 0, len);
            return result;
        }

        // 색 판정: 0이면 초록, 0이 아니면 자리 수(LEN) 짝수 노랑, 홀수 빨강
        static LedColor Judge(byte[] digits)
        {
            if (digits.All(d => d == 0)) return LedColor.Green;
            return digits.Length % 2 == 0 ? LedColor.Yellow : LedColor.Red;
        }

        // 숫자 생성: 자리 수 1~4, 첫 자리 1~9, 한 자리일 때만 0~9
        byte[] RandomDigits()
        {
            int len = rng.Next(1, 5);
            if (len == 1)
            {
                return new byte[] { (byte)rng.Next(0, 10) };
            }
            byte[] digits = new byte[len];
            digits[0] = (byte)rng.Next(1, 10);
            for (int i = 1; i < len; i++)
            {
                digits[i] = (byte)rng.Next(0, 10);
            }
            return digits;
        }

        static string ColorName(LedColor color) => color switch
        {
            LedColor.Green => "초록",
            LedColor.Yellow => "노랑",
            LedColor.Red => "빨강",
            _ => "끔"
        };

        // mask: bit0 초록, bit1 노랑, bit2 빨강
        void SetMask(byte mask)
        {
            GreenLed.Fill = (mask & 0x01) != 0 ? Brushes.LimeGreen : Brushes.LightGray;
            YellowLed.Fill = (mask & 0x02) != 0 ? Brushes.Gold : Brushes.LightGray;
            RedLed.Fill = (mask & 0x04) != 0 ? Brushes.Red : Brushes.LightGray;
        }

        // 색을 바꿀 때 바로 넘어가지 않고, 잠깐 모두 끈 뒤 새 색을 켠다
        async Task ShowColor(LedColor color)
        {
            byte c = (byte)color;
            SetMask(0);
            await Task.Delay(LedOffGapMs);
            SetMask(c == 0 || c > 3 ? (byte)0 : (byte)(1 << (c - 1)));
        }

        // ================= 저장 / 되돌리기 (LED02 방식) =================

        // 저장하기: 설정 + 결과표는 settings.json, 결과표는 logs 폴더의 CSV로 저장
        private void Save_Click(object sender, RoutedEventArgs e)
        {
            string loopcount = RepeatBox.Text;
            string interval = IntervalBox.Text;
            int loopcountValue = int.TryParse(loopcount, out loopcountValue) ? loopcountValue : 0;
            int intervalValue = int.TryParse(interval, out intervalValue) ? intervalValue : 0;

            // 다른 프로그램(엑셀 등)이 파일을 열고 있으면 저장하지 않는다
            if (IsFileOpen(jsonPath))
            {
                MessageBox.Show($"{jsonPath} 파일이 다른 프로그램에서 열려 있습니다. 파일을 닫고 다시 저장해 주세요.");
                return;
            }

            savedSession.Settings.RepeatCount = loopcountValue;
            savedSession.Settings.IntervalMs = intervalValue;
            savedSession.Records = records.ToList();

            try
            {
                // JSON: 되돌리기용
                File.WriteAllText(jsonPath, JsonSerializer.Serialize(savedSession), Encoding.UTF8);

                // CSV: 한 줄 = 한 회차
                Directory.CreateDirectory("logs");
                string logPath = Path.Combine("logs", $"{DateTime.Now:yyyy-MM-dd_HH_mm_ss}.csv");
                List<string> logLines = new List<string>();
                logLines.Add("회차,방식,랜덤 숫자,색,성공 여부,시간");
                foreach (RoundRecord r in records)
                {
                    logLines.Add($"{r.Round},{r.Mode},{r.Number},{r.Color},{(r.Success ? "성공" : "실패")},{r.Time}");
                }
                File.WriteAllLines(logPath, logLines, Encoding.UTF8);

                noticeText = $"저장 완료: {jsonPath}, {logPath}";
                UpdateMeta();
            }
            catch (Exception ex)
            {
                MessageBox.Show($"파일을 닫아주세요.: {ex.Message}");
            }
        }

        // 되돌리기: settings.json을 불러와 횟수, 간격, 결과표를 복원
        private void Revert_Click(object sender, RoutedEventArgs e)
        {
            LoadSettingsToTextBoxes();
        }

        void LoadSettingsToTextBoxes()
        {
            if (!File.Exists(jsonPath))
            {
                MessageBox.Show("저장된 파일이 없습니다.");
                return;
            }
            // 다른 프로그램이 파일을 열고 있으면 불러오지 않는다
            if (IsFileOpen(jsonPath))
            {
                MessageBox.Show($"{jsonPath} 파일이 다른 프로그램에서 열려 있습니다. 파일을 닫고 다시 시도해 주세요.");
                return;
            }
            try
            {
                var json = File.ReadAllText(jsonPath, Encoding.UTF8);
                SavedSession? loaded = JsonSerializer.Deserialize<SavedSession>(json);
                if (loaded == null || loaded.Settings == null || loaded.Records == null)
                {
                    throw new InvalidDataException("저장 파일 내용이 비어 있거나 형식이 맞지 않습니다.");
                }
                savedSession = loaded;
            }
            catch (Exception ex)
            {
                MessageBox.Show($"설정 파일을 읽는 중 오류가 발생했습니다: {ex.Message}");
                return;
            }

            RepeatBox.Text = savedSession.Settings.RepeatCount.ToString();
            IntervalBox.Text = savedSession.Settings.IntervalMs.ToString();
            records.Clear();
            foreach (RoundRecord r in savedSession.Records)
            {
                records.Add(r);
            }
        }

        // 파일이 다른 프로그램에서 열려 있는지 확인: 혼자 쓰기로 열어 보고, 실패하면 열려 있는 것
        static bool IsFileOpen(string path)
        {
            if (!File.Exists(path))
            {
                return false;
            }
            try
            {
                using (FileStream fs = new FileStream(path, FileMode.Open, FileAccess.ReadWrite, FileShare.None))
                {
                }
                return false;
            }
            catch (IOException)
            {
                return true;
            }
            catch (UnauthorizedAccessException)
            {
                return true;
            }
        }

        private void PortBox_SelectionChanged(object sender, System.Windows.Controls.SelectionChangedEventArgs e)
        {

        }
    }
}
