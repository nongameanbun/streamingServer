# nongameanbun - streaming

## 프로젝트 구조
```text
.
├── main.py # WebRTC 시그널링, FastAPI 라우터 및 스트리머 빌드/실행 제어 로직
├── env.example # 환경 변수 예시 파일
├── CMakeLists.txt # C++ 초저지연 스트리머 빌드를 위한 CMake 설정 파일
├── include # C++ 헤더 디렉토리
│   ├── HardwareEncoder.h # NVENC/QSV 하드웨어 인코더
│   ├── ScreenCapture.h # DXGI Desktop Duplication API 기반 화면 캡처
│   ├── SignalingClient.h # 웹소켓 시그널링 통신 클라이언트
│   ├── StreamCommon.h # 공통 타입 및 유틸리티
│   ├── StreamingPipeline.h # 화면 캡처~인코딩~WebRTC 통합 파이프라인
│   └── WebRTCManager.h # WebRTC P2P 스트리밍 제어 관리
└── src # C++ 실행 파일용 소스 디렉토리
    ├── HardwareEncoder.cpp
    ├── ScreenCapture.cpp
    ├── SignalingClient.cpp
    ├── StreamingPipeline.cpp
    ├── WebRTCManager.cpp
    └── main.cpp # C++ 스트리머 엔트리포인트 모듈
```

## 사전 요구 사항

### 환경 변수 세팅 (`.env`)
환경에 맞게 각 포트 번호를 지정하여 프로젝트 루트에 `.env` 파일을 생성합니다.

```powershell
Copy-Item env.example .env
```

`env.example` 포맷 예시:
```ini
RUNE_SOLVER_PORT=8020
inputHandler_API_PORT=8001
statusChecker_API_PORT=8002
alarmHandler_API_PORT=8003
intrAction_API_PORT=8004
mainAction_API_PORT=8005
subaction_API_PORT=8006
streaning_API_PORT=8007
objectDetector_API_PORT=8008
agentServer_API_PORT=8009
```

### 추가 시스템 요구사항 (C++ 스트리머 연동 시)
- Windows 10 이상, NVIDIA (NVENC) 또는 Intel CPU (QuickSync) 하드웨어 가속 지원
- Visual Studio 2019 이상 또는 MSVC 빌드 도구 등 CMake 빌드 환경

## 실행 방법

파이썬 필수 패키지 설치 후 서버를 실행합니다. FastAPI 서버 구동과 함께 C++ 스트리머 자동 빌드가 수행됩니다.

```bash
pip install fastapi uvicorn websockets python-dotenv
python main.py
```

`localhost:8765/docs` 로 swagger 명세를 확인 가능
