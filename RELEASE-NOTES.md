# avio-cast 릴리스 노트

## AVIO-CAST v0.1.0 (2026-09-04)

첫 판. 보드가 `cast.*` 로 티핑하는 카메라 스트림을 PC 에서 **라이브 avio 소스**로 받는다.
보드에 녹화 → 매체로 옮기기 → PC 에서 재생하던 왕복 없이, 카메라 영상이 알고리즘에
곧바로 들어간다.

### 새 기능

- 채널 = 디바이스. `Open("cam0".."cam7")`, `Open("screen")`. 포트는 `base_port + 슬롯`,
  슬롯 0 = 보드 화면, 카메라는 1 부터(avsink `cast.caps` 규약). 기본 base_port 25560.
- `adb forward` 자동 설치·갱신. 포워드가 빠진 모습은 죽은 보드와 구별되지 않으므로
  전송을 아는 쪽이 책임진다.
- 채널당 디코드 스레드. 한 채널이 막혀도 나머지 여덟을 붙잡지 않는다.
- 채널당 8슬롯 프레임 링 + refcount. `RefFrame`/`UnrefFrame` 으로 알고리즘이 프레임을
  여러 장 쥘 수 있다.
- 디코더 백엔드 seam(`cast_dec.h`) — 현재 libavcodec, gst 는 자리만.
- `Do("cast.caps"/"cast.config"/"cast.status"/"adb.forward")`.
- ImGui 상태 패널 — 채널별 링크 상태·해상도·프레임 수·오류.

### 호환성·주의사항

- **Windows 전용.** 보드가 자기 cast 를 받을 이유가 없고 디코더가 PC 쪽 의존이다.
  `FFMPEG_SDK`, `GSTREAMER_1_0_ROOT_MSVC_X86_64` 환경변수를 쓴다.
- **스트림을 켜고 끄지 않는다.** 몇 채널을 흘릴지는 대역폭 판단(ADB USB2 실효
  ~205Mbps)이고 무엇을 보는지는 호스트만 안다. `cast.caps` 의 `controls_stream: false`
  가 이 사실을 계약으로 명시한다. 호스트가 자기 제어 링크로 보낸다.
- 색변환을 하지 않는다. I420/NV12 평면 그대로 올리고 GPU 가 변환한다(픽셀당 1.5B).
- 녹화는 아직 없다. 다만 원시 패킷 탭(`CAST_PACKET_FN`)을 데이터 흐름에 남겨 두었다 —
  보드가 레코더의 패킷을 그대로 티핑하므로 PC 쪽 녹화는 재인코딩이 아니라 mux 다.
