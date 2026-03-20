"""
FastAPI WebSocket Signaling Server for WebRTC

이 서버는 WebRTC P2P 연결을 위한 시그널링을 처리합니다.
- SDP Offer/Answer 교환
- ICE Candidate 교환 (Trickle ICE)
- 룸 기반 피어 관리
- 스트리머 빌드 및 실행 관리

실행: python signaling_server.py
"""

from fastapi import FastAPI, WebSocket, WebSocketDisconnect, HTTPException, BackgroundTasks
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse
from typing import Dict, Set, Optional
from contextlib import asynccontextmanager
import json
import logging
import subprocess
import asyncio
import os
import signal
import sys
from pathlib import Path
from dotenv import load_dotenv

# .env 파일 로드 (프로젝트 루트에서)
load_dotenv(Path(__file__).parent.parent / ".env")

# 로깅 비활성화 (모든 INFO/DEBUG 로그 숨김)
logging.basicConfig(
    level=logging.CRITICAL,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s'
)
logger = logging.getLogger(__name__)
logger.addHandler(logging.NullHandler())
logger.propagate = False

# 프로젝트 경로 설정
PROJECT_ROOT = Path(__file__).parent.absolute()
BUILD_DIR = PROJECT_ROOT / "build"
EXTERNAL_DIR = PROJECT_ROOT / "external"
EXECUTABLE_NAME = "UltraLowLatencyStreamer.exe"
FFMPEG_URL = "https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-full-shared.7z" # Alternative or better: a zip version
# Since 7z might not be available, let's try to find a .zip version or use a simpler one.
# Gyan.dev provides .zip for some builds.
FFMPEG_ZIP_URL = "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl-shared.zip"


class StreamerManager:
    """스트리머 프로세스 관리"""
    
    def __init__(self):
        self.process: Optional[subprocess.Popen] = None
        self.is_built = False
        self.build_log: list = []
        self.streamer_log: list = []
        self._log_task: Optional[asyncio.Task] = None
        self.cmake_available = self._check_cmake()
        self.current_params: dict = {
            "x": 0,
            "y": 30,
            "width": 1366,
            "height": 768,
            "fps": 60,
            "bitrate": 40000
        }
    
    def _check_cmake(self) -> bool:
        """CMake 사용 가능 여부 확인"""
        # 1. 환경변수에서 CMAKE_PATH 확인
        cmake_path = os.environ.get("CMAKE_PATH")
        
        # 2. 기본 설치 경로 확인
        if not cmake_path:
            common_paths = [
                r"C:\Program Files\CMake\bin\cmake.exe",
                r"C:\Program Files (x86)\CMake\bin\cmake.exe",
            ]
            for p in common_paths:
                if Path(p).exists():
                    cmake_path = p
                    break
        
        # 3. cmake 명령어로 시도
        cmake_cmd = cmake_path if cmake_path else "cmake"
        
        try:
            # Windows에서 shell=True로 실행
            result = subprocess.run(
                f'"{cmake_cmd}" --version' if cmake_path else "cmake --version",
                capture_output=True,
                text=True,
                timeout=10,
                shell=True
            )
            if result.returncode == 0:
                version_line = result.stdout.split('\n')[0].strip() if result.stdout else "unknown"
                logger.info(f"CMake detected: {version_line}")
                # cmake_path를 저장해서 나중에 사용
                if cmake_path:
                    self._cmake_path = cmake_path
                return True
            return False
        except Exception as e:
            logger.warning(f"CMake check failed: {e}")
            return False

    def _find_openssl(self) -> Optional[str]:
        """Windows에서 OpenSSL 설치 경로 찾기"""
        if sys.platform != 'win32':
            return None
            
        # 1. common paths
        common_paths = [
            r"C:\Program Files\OpenSSL-Win64",
            r"C:\Program Files\FireDaemon OpenSSL 3",
            r"C:\Program Files (x86)\OpenSSL-Win32",
        ]
        for p in common_paths:
            if (Path(p) / "include" / "openssl" / "ssl.h").exists():
                return p
                
        # 2. environment variable
        if "OPENSSL_ROOT_DIR" in os.environ:
            return os.environ["OPENSSL_ROOT_DIR"]
            
        # 3. where openssl
        try:
            result = subprocess.run(["where", "openssl"], capture_output=True, text=True)
            if result.returncode == 0:
                first_line = result.stdout.strip().split('\n')[0]
                bin_dir = Path(first_line).parent
                root_dir = bin_dir.parent
                if (root_dir / "include" / "openssl" / "ssl.h").exists():
                    return str(root_dir)
                # handle cases like Library/bin/openssl.exe (Anaconda)
                elif (root_dir / ".." / "include" / "openssl" / "ssl.h").exists():
                    return str(root_dir.parent)
        except: pass
        
        return None

    async def _ensure_ffmpeg(self) -> bool:
        """Windows에서 FFmpeg Shared 빌드가 있는지 확인하고 없으면 다운로드"""
        if sys.platform != "win32":
            return True # Linux/Mac은 패키지 매니저 권장
            
        ffmpeg_dir = EXTERNAL_DIR / "ffmpeg"
        if (ffmpeg_dir / "include" / "libavcodec" / "avcodec.h").exists():
            return True
            
        print(f"[StreamerManager] FFmpeg not found. Downloading to {ffmpeg_dir}...")
        EXTERNAL_DIR.mkdir(exist_ok=True, parents=True)
        
        import urllib.request
        import zipfile
        import shutil
        import tempfile

        try:
            with tempfile.TemporaryDirectory() as tmpdir:
                zip_path = Path(tmpdir) / "ffmpeg.zip"
                print(f"[StreamerManager] Downloading FFmpeg from {FFMPEG_ZIP_URL}...")
                urllib.request.urlretrieve(FFMPEG_ZIP_URL, zip_path)
                
                print("[StreamerManager] Extracting FFmpeg...")
                with zipfile.ZipFile(zip_path, 'r') as zip_ref:
                    zip_ref.extractall(tmpdir)
                
                # 추출된 폴더 찾기 (보통 ffmpeg-master-latest-win64-shared/ 형태)
                extracted_folders = [p for p in Path(tmpdir).iterdir() if p.is_dir() and "ffmpeg" in p.name.lower()]
                if not extracted_folders:
                    raise Exception("Could not find extracted FFmpeg folder")
                
                source_folder = extracted_folders[0]
                
                if ffmpeg_dir.exists():
                    shutil.rmtree(ffmpeg_dir)
                
                shutil.move(str(source_folder), str(ffmpeg_dir))
                print(f"[StreamerManager] FFmpeg setup complete at {ffmpeg_dir}")
                return True
        except Exception as e:
            print(f"[StreamerManager] Failed to download FFmpeg: {e}")
            return False
    
    def _check_existing_build(self) -> bool:
        """기존 빌드 파일 확인"""
        exe_path = self._find_executable()
        if exe_path and exe_path.exists():
            self.is_built = True
            return True
        return False
    
    async def build(self, force: bool = False) -> dict:
        """CMake 빌드 수행"""
        # 기존 빌드 확인
        if not force and self._check_existing_build():
            return {"status": "already_built", "message": "기존 빌드 파일이 있습니다."}
        
        if self.is_built and not force:
            return {"status": "already_built", "message": "이미 빌드되어 있습니다."}
        
        # CMake 확인
        if not self.cmake_available:
            return {
                "status": "skipped",
                "message": "CMake가 설치되어 있지 않습니다. 수동으로 빌드하세요."
            }
        
        self.build_log = []
        
        try:
            # 0. FFmpeg 경로 확인 (.env에서 읽거나 기본값 사용)
            ffmpeg_root = os.environ.get("FFMPEG_ROOT")
            if not ffmpeg_root:
                # 기본 경로 시도
                default_paths = [
                    "C:/ffmpeg",
                    str(EXTERNAL_DIR / "ffmpeg"),
                ]
                for p in default_paths:
                    if Path(p).exists() and (Path(p) / "include" / "libavcodec" / "avcodec.h").exists():
                        ffmpeg_root = p
                        break
            
            if not ffmpeg_root or not Path(ffmpeg_root).exists():
                # 자동 다운로드 시도
                if not await self._ensure_ffmpeg():
                    return {"status": "error", "message": "FFmpeg not found. Set FFMPEG_ROOT in .env or install FFmpeg."}
                ffmpeg_root = str(EXTERNAL_DIR / "ffmpeg")

            logger.info(f"Using FFmpeg at: {ffmpeg_root}")

            # 빌드 디렉토리 생성
            BUILD_DIR.mkdir(exist_ok=True, parents=True)
            
            logger.info("CMake 구성 시작...")
            self.build_log.append("=== CMake Configure ===")
            
            # CMake 구성
            cmake_args = [
                "cmake", "..", 
                "-DCMAKE_BUILD_TYPE=Release",
                f"-DFFMPEG_ROOT={ffmpeg_root}"
            ]
            
            # Windows에서 Visual Studio Generator 사용
            if sys.platform == 'win32':
                cmake_args.extend(["-G", "Visual Studio 17 2022", "-A", "x64"])
            
            # OpenSSL 경로 추가
            openssl_root = os.environ.get("OPENSSL_ROOT_DIR") or self._find_openssl()
            if openssl_root:
                logger.info(f"OpenSSL found at: {openssl_root}")
                cmake_args.append(f"-DOPENSSL_ROOT_DIR={openssl_root}")
            else:
                logger.warning("OpenSSL not found. Build might fail.")
            
            # Use Ninja if available (only on non-Windows)
            try:
                if sys.platform != 'win32' and subprocess.run(["ninja", "--version"], capture_output=True).returncode == 0:
                    cmake_args.extend(["-G", "Ninja"])
            except: pass

            # CMake 경로 사용 (_check_cmake에서 저장된 경로 또는 기본값)
            cmake_cmd = getattr(self, '_cmake_path', None) or "cmake"
            cmake_args[0] = cmake_cmd  # cmake 명령어를 절대 경로로 교체
            
            logger.info(f"Running CMake configure: {' '.join(cmake_args[:3])}...")
            
            cmake_config = await asyncio.create_subprocess_exec(
                *cmake_args,
                cwd=str(BUILD_DIR),
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.STDOUT
            )
            stdout, _ = await cmake_config.communicate()
            self.build_log.append(stdout.decode('utf-8', errors='ignore'))
            
            if cmake_config.returncode != 0:
                return {
                    "status": "error",
                    "message": "CMake 구성 실패",
                    "log": self.build_log
                }
            
            logger.info("빌드 시작...")
            self.build_log.append("\n=== CMake Build ===")
            
            # 빌드 실행 (cmake 경로 사용)
            cmake_build = await asyncio.create_subprocess_exec(
                cmake_cmd, "--build", ".", "--config", "Release",
                cwd=str(BUILD_DIR),
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.STDOUT
            )
            stdout, _ = await cmake_build.communicate()
            self.build_log.append(stdout.decode('utf-8', errors='ignore'))
            
            if cmake_build.returncode != 0:
                return {
                    "status": "error", 
                    "message": "빌드 실패",
                    "log": self.build_log
                }
            
            self.is_built = True
            logger.info("빌드 완료!")
            
            return {
                "status": "success",
                "message": "빌드 완료",
                "log": self.build_log
            }
            
        except FileNotFoundError as e:
            error_msg = f"CMake를 찾을 수 없습니다: {e}"
            logger.error(error_msg)
            return {"status": "error", "message": error_msg}
        except Exception as e:
            error_msg = f"빌드 중 오류 발생: {e}"
            logger.error(error_msg)
            return {"status": "error", "message": error_msg}
    
    def _find_executable(self) -> Optional[Path]:
        """빌드된 실행 파일 찾기"""
        # 1. 환경 변수에서 직접 경로 지정한 경우 우선 사용
        env_path = os.environ.get("STREAMER_EXECUTABLE_PATH")
        if env_path:
            env_path_obj = Path(env_path)
            if env_path_obj.exists():
                logger.info(f"Found executable from STREAMER_EXECUTABLE_PATH: {env_path_obj}")
                return env_path_obj
            else:
                logger.warning(f"STREAMER_EXECUTABLE_PATH set but file not found: {env_path}")
        
        # 2. 가능한 경로들 검색
        possible_paths = [
            BUILD_DIR / "Release" / EXECUTABLE_NAME,
            BUILD_DIR / EXECUTABLE_NAME,
            BUILD_DIR / "Debug" / EXECUTABLE_NAME,
            PROJECT_ROOT / EXECUTABLE_NAME,
            Path.cwd() / EXECUTABLE_NAME,
        ]
        
        logger.debug(f"Searching for executable in paths:")
        for path in possible_paths:
            logger.debug(f"  - {path} (exists: {path.exists()})")
            if path.exists():
                logger.info(f"Found executable at: {path}")
                return path
        
        logger.warning(f"Executable not found. PROJECT_ROOT={PROJECT_ROOT}, CWD={Path.cwd()}")
        logger.warning("Tip: Set STREAMER_EXECUTABLE_PATH in .env file to specify the path directly.")
        return None
    
    async def start(self, room_id: str = "default", **kwargs) -> dict:
        """스트리머 시작 (이미 실행 중이면 자동 재시작)"""
        # 이미 실행 중이면 먼저 중지
        if self.process and self.process.poll() is None:
            logger.info("기존 스트리머 중지 후 재시작...")
            await self.stop()
        
        exe_path = self._find_executable()
        if not exe_path:
            return {
                "status": "error",
                "message": f"실행 파일을 찾을 수 없습니다. 먼저 빌드하세요.",
                "searched_paths": [str(BUILD_DIR / "Release"), str(BUILD_DIR)]
            }
        
        # 명령줄 인자 구성
        args = [str(exe_path)]
        args.extend(["--room", room_id])
        args.extend(["--server", f"ws://localhost:8765/ws"])
        
        # 캡처 영역 옵션 (기본값 설정: x=0, y=30)
        x = kwargs.get("x", 0)
        y = kwargs.get("y", 30)
        width = kwargs.get("width", 1366)
        height = kwargs.get("height", 768)
        fps = kwargs.get("fps", 60)
        bitrate = kwargs.get("bitrate", 40000)  # 40 Mbps for maximum sharpness

        self.current_params = {
            "x": x,
            "y": y,
            "width": width,
            "height": height,
            "fps": fps,
            "bitrate": bitrate
        }

        args.extend(["--x", str(x)])
        args.extend(["--y", str(y)])
        args.extend(["--width", str(width)])
        args.extend(["--height", str(height)])
        args.extend(["--fps", str(fps)])
        args.extend(["--bitrate", str(bitrate)])
        
        # TURN 서버 설정 추가
        turn_url = os.environ.get("TURN_SERVER_URL")
        turn_user = os.environ.get("TURN_USERNAME")
        turn_pass = os.environ.get("TURN_CREDENTIAL")
        
        if turn_url:
            args.extend(["--turn-url", turn_url])
            if turn_user:
                args.extend(["--turn-user", turn_user])
            if turn_pass:
                args.extend(["--turn-pass", turn_pass])
        
        try:
            logger.info(f"스트리머 시작: {' '.join(args)}")
            self.streamer_log = []
            
            # 프로세스 시작
            self.process = subprocess.Popen(
                args,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                creationflags=subprocess.CREATE_NEW_PROCESS_GROUP if sys.platform == 'win32' else 0
            )
            
            # 로그 수집 태스크 시작
            self._log_task = asyncio.create_task(self._collect_logs())
            
            return {
                "status": "started",
                "message": "스트리머가 시작되었습니다.",
                "pid": self.process.pid,
                "room_id": room_id
            }
            
        except Exception as e:
            error_msg = f"스트리머 시작 실패: {e}"
            logger.error(error_msg)
            return {"status": "error", "message": error_msg}
    
    async def _collect_logs(self):
        """스트리머 로그 수집"""
        if not self.process or not self.process.stdout:
            return
        
        try:
            while self.process.poll() is None:
                line = await asyncio.get_event_loop().run_in_executor(
                    None, self.process.stdout.readline
                )
                if line:
                    log_line = line.decode('utf-8', errors='ignore').strip()
                    self.streamer_log.append(log_line)
                    logger.info(f"[Streamer] {log_line}")
                    # 최근 100줄만 유지
                    if len(self.streamer_log) > 100:
                        self.streamer_log.pop(0)
                else:
                    await asyncio.sleep(0.1)
        except Exception as e:
            logger.error(f"로그 수집 오류: {e}")
    
    async def stop(self) -> dict:
        """스트리머 중지"""
        if not self.process or self.process.poll() is not None:
            return {"status": "not_running", "message": "스트리머가 실행 중이 아닙니다."}
        
        try:
            logger.info("스트리머 중지 중...")
            
            # Windows에서는 CTRL_BREAK_EVENT 전송
            if sys.platform == 'win32':
                self.process.send_signal(signal.CTRL_BREAK_EVENT)
            else:
                self.process.terminate()
            
            # 3초 대기
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                logger.warning("강제 종료...")
                self.process.kill()
                self.process.wait()
            
            if self._log_task:
                self._log_task.cancel()
            
            return {"status": "stopped", "message": "스트리머가 중지되었습니다."}
            
        except Exception as e:
            error_msg = f"스트리머 중지 실패: {e}"
            logger.error(error_msg)
            return {"status": "error", "message": error_msg}
    
    def get_status(self) -> dict:
        """스트리머 상태 조회"""
        is_running = self.process is not None and self.process.poll() is None
        exe_path = self._find_executable()
        
        return {
            "is_built": self.is_built,
            "is_running": is_running,
            "pid": self.process.pid if is_running else None,
            "executable_path": str(exe_path) if exe_path else None,
            "cmake_available": self.cmake_available,
            "recent_logs": self.streamer_log[-20:] if self.streamer_log else []
        }


# 스트리머 매니저 인스턴스
streamer_manager = StreamerManager()


@asynccontextmanager
async def lifespan(app: FastAPI):
    """서버 시작/종료 시 실행"""
    logger.info("=" * 50)
    logger.info("서버 시작...")
    logger.info("=" * 50)
    
    # 실행 파일 존재 여부 먼저 확인
    exe_exists = streamer_manager._check_existing_build()
    
    # 실행 파일이 없으면 빌드 시도
    if not exe_exists:
        logger.info("실행 파일이 없습니다. 빌드를 시도합니다...")
        
        # 1. 환경변수에서 빌드 커맨드 확인
        build_command = os.environ.get("STREAMER_BUILD_COMMAND")
        
        if build_command:
            # .env에 지정된 빌드 커맨드 실행
            logger.info(f"STREAMER_BUILD_COMMAND 실행: {build_command}")
            try:
                build_cwd = os.environ.get("STREAMER_BUILD_CWD", str(PROJECT_ROOT / "streaming"))
                process = await asyncio.create_subprocess_shell(
                    build_command,
                    cwd=build_cwd,
                    stdout=asyncio.subprocess.PIPE,
                    stderr=asyncio.subprocess.STDOUT
                )
                stdout, _ = await process.communicate()
                output = stdout.decode('utf-8', errors='ignore')
                
                if process.returncode == 0:
                    logger.info("✓ 외부 빌드 커맨드 성공!")
                    exe_exists = streamer_manager._check_existing_build()
                else:
                    logger.error(f"✗ 외부 빌드 커맨드 실패 (exit code: {process.returncode})")
                    for line in output.split('\n')[-10:]:
                        if line.strip():
                            logger.error(line.strip())
            except Exception as e:
                logger.error(f"✗ 빌드 커맨드 실행 실패: {e}")
        
        # 2. CMake 사용 가능하면 CMake 빌드 (런타임에 다시 체크)
        # 초기화 시점에 cmake_available이 False여도 런타임에 다시 체크
        streamer_manager.cmake_available = streamer_manager._check_cmake()
        
        if streamer_manager.cmake_available:
            logger.info("CMake 감지됨 - 자동 빌드 시작...")
            result = await streamer_manager.build(force=True)
            if result["status"] == "success":
                logger.info("✓ 빌드 성공!")
                exe_exists = True
            else:
                logger.error(f"✗ 빌드 실패: {result.get('message')}")
                if "log" in result:
                    log_summary = result["log"][-10:] if isinstance(result["log"], list) else str(result["log"]).split('\n')[-10:]
                    logger.error("--- Build Log Tail ---")
                    for line in log_summary:
                        logger.error(line.strip())
                    logger.error("---------------------")
        else:
            logger.warning("CMake가 설치되어 있지 않습니다.")
            logger.warning("Tip: .env 파일에 STREAMER_BUILD_COMMAND를 설정하세요.")
            logger.warning("예: STREAMER_BUILD_COMMAND=cmake --build build --config Release")
    else:
        logger.info("✓ 기존 빌드 파일 확인 완료")
    
    # 빌드된 파일이 있으면 자동 시작
    if exe_exists or streamer_manager._check_existing_build():
        logger.info("스트리머를 백그라운드에서 자동 실행합니다.")
        await streamer_manager.start(room_id="default")
    else:
        logger.warning("✗ 빌드된 스트리머를 찾을 수 없습니다.")
        logger.warning("Tip: .env 파일에 다음을 설정하세요:")
        logger.warning("  STREAMER_EXECUTABLE_PATH=<실행파일 경로>")
        logger.warning("  STREAMER_BUILD_COMMAND=<빌드 명령어>")
        logger.warning("  STREAMER_BUILD_CWD=<빌드 작업 디렉토리>")

    logger.info("=" * 50)
    logger.info("서버 준비 완료! http://localhost:8000")
    logger.info("API 문서: http://localhost:8000/docs")
    logger.info("=" * 50)
    
    yield
    
    # 종료 시 스트리머 정리
    logger.info("서버 종료 중...")
    await streamer_manager.stop()

app = FastAPI(
    title="WebRTC Signaling Server",
    description="WebRTC 시그널링 및 스트리머 관리 서버",
    lifespan=lifespan
)

# CORS 설정
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

# 연결된 피어 관리
class ConnectionManager:
    def __init__(self):
        # room_id -> {peer_id -> WebSocket}
        self.rooms: Dict[str, Dict[str, WebSocket]] = {}
        # WebSocket -> (room_id, peer_id)
        self.connections: Dict[WebSocket, tuple] = {}
        # peer_id -> role
        self.roles: Dict[str, str] = {}
    
    async def connect(self, websocket: WebSocket, room_id: str, peer_id: str):
        await websocket.accept()
        
        # 룸에 추가
        if room_id not in self.rooms:
            self.rooms[room_id] = {}
        
        self.rooms[room_id][peer_id] = websocket
        self.connections[websocket] = (room_id, peer_id)
        
        logger.info(f"Peer {peer_id} joined room {room_id}")
        
        # 환영 메시지
        await websocket.send_json({
            "type": "welcome",
            "roomId": room_id,
            "peerId": peer_id
        })
        
        # 기존 피어들에게 알림
        await self.broadcast_to_room(room_id, {
            "type": "peer_joined",
            "roomId": room_id,
            "peerId": peer_id
        }, exclude=peer_id)
        
        # 신규 피어에게 기존 피어 목록 전송
        existing_peers = [pid for pid in self.rooms[room_id].keys() if pid != peer_id]
        if existing_peers:
            await websocket.send_json({
                "type": "peers",
                "roomId": room_id,
                "peers": existing_peers
            })
    
    def disconnect(self, websocket: WebSocket):
        if websocket in self.connections:
            room_id, peer_id = self.connections[websocket]
            
            # 룸에서 제거
            if room_id in self.rooms and peer_id in self.rooms[room_id]:
                del self.rooms[room_id][peer_id]
                
                # 빈 룸 정리
                if not self.rooms[room_id]:
                    del self.rooms[room_id]
            
            if peer_id in self.roles:
                del self.roles[peer_id]
            
            del self.connections[websocket]
            logger.info(f"Peer {peer_id} left room {room_id}")
            
            return room_id, peer_id
        return None, None
    
    async def broadcast_to_room(self, room_id: str, message: dict, exclude: str = None):
        if room_id in self.rooms:
            for peer_id, websocket in self.rooms[room_id].items():
                if peer_id != exclude:
                    try:
                        await websocket.send_json(message)
                    except Exception as e:
                        logger.error(f"Failed to send to {peer_id}: {e}")
    
    async def send_to_peer(self, room_id: str, peer_id: str, message: dict):
        if room_id in self.rooms and peer_id in self.rooms[room_id]:
            try:
                await self.rooms[room_id][peer_id].send_json(message)
                return True
            except Exception as e:
                logger.error(f"Failed to send to {peer_id}: {e}")
        return False
    
    def get_peer_websocket(self, room_id: str, peer_id: str) -> WebSocket:
        if room_id in self.rooms:
            return self.rooms[room_id].get(peer_id)
        return None


manager = ConnectionManager()


@app.get("/")
async def root():
    return {
        "message": "WebRTC Signaling Server",
        "status": "running",
        "endpoints": {
            "signaling": "/ws",
            "health": "/health",
            "build": "/build",
            "start": "/streamer/start",
            "stop": "/streamer/stop",
            "status": "/streamer/status",
            "logs": "/streamer/logs",
            "rooms": "/rooms"
        }
    }


@app.get("/health")
async def health():
    status = streamer_manager.get_status()
    return {
        "status": "healthy",
        "streamer": status
    }


# ============================================================================
# 스트리머 관리 API
# ============================================================================

@app.post("/build")
async def build_streamer(force: bool = False):
    """
    스트리머 빌드
    
    - force: 강제 재빌드 여부
    """
    result = await streamer_manager.build(force=force)
    
    if result["status"] == "error":
        return JSONResponse(
            status_code=500,
            content={"resp": -1, "message": result.get("message", "빌드 실패"), "log": result.get("log", [])}
        )
    
    return {"resp": 0, "message": result.get("message", "빌드 완료"), **result}


@app.post("/streamer/start")
async def start_streamer(
    room_id: str = "default",
    x: int = 0,
    y: int = 30,
    width: int = 1366,
    height: int = 768,
    fps: int = 60,
    bitrate: int = 8000
):
    """
    스트리머 시작 (영역 캡처 지원)
    
    - room_id: 참가할 룸 ID
    - x: 캡처 영역 X 오프셋
    - y: 캡처 영역 Y 오프셋
    - width: 캡처 너비
    - height: 캡처 높이
    - fps: 목표 프레임레이트
    - bitrate: 비트레이트 (kbps)
    """
    result = await streamer_manager.start(
        room_id=room_id,
        x=x,
        y=y,
        width=width,
        height=height,
        fps=fps,
        bitrate=bitrate
    )
    
    if result["status"] == "error":
        return JSONResponse(
            status_code=500,
            content={"resp": -1, "message": result.get("message", "시작 실패")}
        )
    
    return {"resp": 0, "message": result.get("message", "시작 완료"), **result}


@app.post("/streamer/stop")
async def stop_streamer():
    """스트리머 중지"""
    result = await streamer_manager.stop()
    return result


@app.get("/streamer/status")
async def get_streamer_status():
    """스트리머 상태 조회"""
    return streamer_manager.get_status()


@app.get("/streamer/info", summary="스트리머 캡처 정보 조회")
async def get_streamer_info():
    """현재 설정된 스트리머 캡처 영역 정보를 반환합니다."""
    return {
        "resp": 0,
        "data": streamer_manager.current_params
    }


@app.get("/streamer/logs")
async def get_streamer_logs():
    """스트리머 로그 조회"""
    return {
        "build_log": streamer_manager.build_log[-50:],
        "streamer_log": streamer_manager.streamer_log[-50:]
    }


# ============================================================================
# 피어 연결 관리
# ============================================================================


@app.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket):
    """
    WebSocket 엔드포인트
    
    메시지 형식:
    - join: {"type": "join", "roomId": "...", "peerId": "...", "role": "sender|receiver"}
    - leave: {"type": "leave"}
    - offer: {"type": "offer", "toPeerId": "...", "sdp": "..."}
    - answer: {"type": "answer", "toPeerId": "...", "sdp": "..."}
    - candidate: {"type": "candidate", "toPeerId": "...", "candidate": {...}}
    - ping: {"type": "ping"} - heartbeat to keep connection alive
    """
    # 임시 연결 (join 메시지 대기)
    await websocket.accept()
    
    room_id = None
    peer_id = None
    
    # Ping-Pong keepalive mechanism
    PING_INTERVAL = 20  # seconds
    
    async def ping_task():
        """Background task to send ping periodically"""
        while True:
            try:
                await asyncio.sleep(PING_INTERVAL)
                await websocket.send_json({"type": "ping"})
            except Exception:
                break
    
    ping_task_handle = asyncio.create_task(ping_task())
    
    try:
        while True:
            # Use wait_for with longer timeout to prevent disconnect
            try:
                data = await asyncio.wait_for(websocket.receive_text(), timeout=60)
            except asyncio.TimeoutError:
                # Send ping on timeout to keep connection alive
                try:
                    await websocket.send_json({"type": "ping"})
                    continue
                except:
                    break
                    
            message = json.loads(data)
            # 모든 메시지 로깅 (디버깅용)
            if message.get("type") not in ["ping", "pong"]:
                logger.info(f"Signaling Message from {peer_id if 'peer_id' in locals() else 'unknown'}: {message}")
            
            msg_type = message.get("type", "")
            
            # Handle ping/pong
            if msg_type == "ping":
                await websocket.send_json({"type": "pong"})
                continue
            elif msg_type == "pong":
                continue
            
            if msg_type == "join":
                # 룸 참가
                room_id = message.get("roomId", "default")
                peer_id = message.get("peerId", f"peer_{id(websocket)}")
                role = message.get("role", "unknown")
                
                # Receiver 참가 시 처리
                if role == "receiver":
                    # 스트리머 프로세스 확인 및 스마트 스타트
                    status = streamer_manager.get_status()
                    if not status["is_running"]:
                        logger.info(f"Receiver {peer_id} joined. Starting streamer process...")
                        await streamer_manager.start(room_id=room_id)
                    else:
                        logger.info(f"Streamer already running. C++ app will handle new peer {peer_id}.")

                # 룸 및 연결 관리 등록
                if websocket in manager.connections:
                    manager.disconnect(websocket)
                
                if room_id not in manager.rooms:
                    manager.rooms[room_id] = {}
                
                manager.rooms[room_id][peer_id] = websocket
                manager.connections[websocket] = (room_id, peer_id)
                manager.roles[peer_id] = role
                
                logger.info(f"Peer {peer_id} ({role}) joined room {room_id}")
                
                # 환영 메시지
                await websocket.send_json({
                    "type": "joined",
                    "roomId": room_id,
                    "peerId": peer_id
                })
                
                # 기존 피어들에게 알림
                await manager.broadcast_to_room(room_id, {
                    "type": "peer_joined",
                    "roomId": room_id,
                    "peerId": peer_id,
                    "role": role
                }, exclude=peer_id)
                
                # 신규 피어에게 기존 피어 목록 전송
                existing_peers = [pid for pid in manager.rooms[room_id].keys() if pid != peer_id]
                if existing_peers:
                    for existing_peer_id in existing_peers:
                        await websocket.send_json({
                            "type": "peer_joined",
                            "roomId": room_id,
                            "peerId": existing_peer_id,
                            "role": manager.roles.get(existing_peer_id, "unknown")
                        })
            
            elif msg_type == "leave":
                # 룸 떠남
                left_room_id, left_peer_id = manager.disconnect(websocket)
                if left_room_id:
                    await manager.broadcast_to_room(left_room_id, {
                        "type": "peer_left",
                        "roomId": left_room_id,
                        "peerId": left_peer_id
                    })
                break
            
            elif msg_type == "offer":
                # SDP Offer 전달
                to_peer_id = message.get("toPeerId", "")
                sdp = message.get("sdp", "")
                
                logger.info(f"Received OFFER from {peer_id} for {to_peer_id}")
                if room_id and to_peer_id:
                    success = await manager.send_to_peer(room_id, to_peer_id, {
                        "type": "offer",
                        "fromPeerId": peer_id,
                        "sdp": sdp
                    })
                    logger.info(f"Offer from {peer_id} to {to_peer_id}: {'sent' if success else 'failed'}")
                else:
                    logger.warning(f"Invalid offer: room_id={room_id}, to_peer_id={to_peer_id}")
            
            elif msg_type == "answer":
                # SDP Answer 전달
                to_peer_id = message.get("toPeerId", "")
                sdp = message.get("sdp", "")
                
                logger.info(f"Received ANSWER from {peer_id} for {to_peer_id}")
                if room_id and to_peer_id:
                    success = await manager.send_to_peer(room_id, to_peer_id, {
                        "type": "answer",
                        "fromPeerId": peer_id,
                        "sdp": sdp
                    })
                    logger.info(f"Answer from {peer_id} to {to_peer_id}: {'sent' if success else 'failed'}")
                else:
                    logger.warning(f"Invalid answer: room_id={room_id}, to_peer_id={to_peer_id}")
            
            elif msg_type == "candidate":
                # ICE Candidate 전달 (Trickle ICE)
                to_peer_id = message.get("toPeerId", "")
                candidate = message.get("candidate", {})
                
                logger.info(f"Received CANDIDATE from {peer_id} for {to_peer_id}")
                if room_id and to_peer_id:
                    success = await manager.send_to_peer(room_id, to_peer_id, {
                        "type": "candidate",
                        "fromPeerId": peer_id,
                        "candidate": candidate
                    })
                    if not success:
                        logger.warning(f"Failed to send candidate from {peer_id} to {to_peer_id}")
                    else:
                        logger.info(f"Candidate from {peer_id} to {to_peer_id}: sent")
            
            else:
                logger.warning(f"Unknown message type: {msg_type}")
    
    except WebSocketDisconnect:
        logger.info(f"WebSocket disconnected: {peer_id}")
    except json.JSONDecodeError as e:
        logger.error(f"JSON decode error: {e}")
    except OSError as e:
        logger.warning(f"WebSocket OS error (client likely disconnected): {e}")
    except Exception as e:
        logger.error(f"WebSocket error: {e}")
    finally:
        # Cancel ping task
        ping_task_handle.cancel()
        try:
            await ping_task_handle
        except asyncio.CancelledError:
            pass
        
        # 연결 정리
        left_room_id, left_peer_id = manager.disconnect(websocket)
        if left_room_id:
            await manager.broadcast_to_room(left_room_id, {
                "type": "peer_left",
                "roomId": left_room_id,
                "peerId": left_peer_id
            })
            
            # 모든 Receiver가 나갔으면 스트리머 종료 (GPU 자원 절약)
            remaining_receivers = [
                pid for pid, role in manager.roles.items() 
                if role == "receiver" and pid in manager.rooms.get(left_room_id, {})
            ]
            if not remaining_receivers:
                logger.info(f"No receivers left in room {left_room_id}. Stopping streamer...")
                await streamer_manager.stop()


@app.get("/rooms")
async def list_rooms():
    """현재 룸 목록"""
    return {
        "rooms": {
            room_id: list(peers.keys()) 
            for room_id, peers in manager.rooms.items()
        }
    }


# ============================================================================
# 메인 실행
# ============================================================================

def kill_port_owners(ports):
    """지정된 포트를 사용하는 프로세스 종료 (Windows 전용)"""
    import subprocess
    import re
    import os
    
    logger.info(f"Checking ports: {ports}")
    for port in ports:
        try:
            # netstat 출력에서 해당 포트를 사용하는 PID 찾기
            output = subprocess.check_output(f'netstat -ano | findstr ":{port}"', shell=True).decode()
            # "TCP    0.0.0.0:8765           0.0.0.0:0              LISTENING       1234" 형식에서 PID 추출
            for line in output.strip().split('\n'):
                if f":{port}" in line and "LISTENING" in line:
                    parts = line.split()
                    if len(parts) >= 5:
                        pid = parts[-1]
                        if pid.isdigit() and int(pid) != os.getpid():
                            logger.warning(f"Killing process {pid} using port {port}")
                            subprocess.run(f"taskkill /F /PID {pid}", shell=True, capture_output=True)
        except Exception:
            pass

def streaming():
    """스트리밍 서버 시작 (동기 함수)"""
    import uvicorn
    

    # print("""
    # ============================================================
    #          Ultra Low Latency Streamer - Control Server          
    # ============================================================
    # API Endpoints:
    #   POST /build           - Streamer Build
    #   POST /streamer/start  - Streamer Start
    #   POST /streamer/stop   - Streamer Stop
    #   GET  /streamer/status - Status Check
    #   GET  /rooms           - Room List
    #   WS   /ws              - WebRTC Signaling
    # ============================================================
    # """)
    
    uvicorn.run(
        app, 
        host="0.0.0.0", 
        port=8765,
        log_level="warning"
    )


if __name__ == "__main__":
    streaming()
