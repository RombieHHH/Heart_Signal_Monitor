# -*- coding: utf-8 -*-
"""
ecg_host.ecg.server
===================

基于 FastAPI 的 Web 后端。

- REST 接口：GET /api/snapshot、/api/ports、/api/session、/health。
- WebSocket /ws：
    * 客户端发送 { "type": "subscribe" } 订阅后，按固定周期（100 ms）广播快照；
    * 未订阅的连接在短暂等待后推送一次快照并关闭。
- 页面：/ 返回 main.py 传入的 web/index.html，缺文件时退回占位页。

约定数据格式：见 datastore.snapshot() 返回的 JSON 结构。
"""

import asyncio
import json
import logging
from contextlib import asynccontextmanager
from typing import List, Optional

from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.responses import HTMLResponse

from .datastore import DataStore
from .reader import list_serial_ports

log = logging.getLogger("ecg.server")

# 浏览器按 100 ms 刷新，与紧凑协议帧周期一致。
DEFAULT_BROADCAST_MS = 100
# 等待订阅消息的时长；超时视为“只要一次快照”的连接
SUBSCRIBE_TIMEOUT_S = 2.0

# 前端页面缺失时的兜底页
PLACEHOLDER = (
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<title>ECG 上位机</title></head><body>"
    "<h1>ECG 上位机后端已运行</h1>"
    "<p>未找到前端页面 <code>web/index.html</code>，"
    "实时数据可从 <code>/api/snapshot</code>（REST）或 "
    "<code>/ws</code>（WebSocket）获取。</p>"
    "</body></html>"
)


def _load_index(index_file: Optional[str]) -> str:
    """读取前端页面文本；失败或无配置时返回占位页。"""
    if not index_file:
        return PLACEHOLDER
    try:
        with open(index_file, "r", encoding="utf-8") as fh:
            return fh.read()
    except OSError as exc:
        log.warning("cannot read frontend page %s: %s", index_file, exc)
    return PLACEHOLDER


class WebServer:
    """管理 FastAPI 实例、WebSocket 连接与后台广播任务。"""

    def __init__(self, store: DataStore, index_file: Optional[str] = None,
                 broadcast_ms: int = DEFAULT_BROADCAST_MS):
        self.store = store
        self.broadcast_delta = broadcast_ms / 1000.0
        self._index_html = _load_index(index_file)
        self._clients: List[WebSocket] = []
        self.app = FastAPI(title="ECG 上位机后端", version="1.0.0",
                           lifespan=self._lifespan)
        self._setup_routes()

    # ---- 路由 ----
    def _setup_routes(self):
        app = self.app

        @app.get("/health", tags=["system"])
        async def health():
            return {"status": "ok", "service": "ecg-host"}

        @app.get("/api/ports", tags=["api"])
        async def ports():
            items = list_serial_ports()
            return {"ports": [{"port": d, "description": desc} for d, desc in items]}

        @app.get("/api/snapshot", tags=["api"])
        async def snapshot():
            return self.store.snapshot()

        @app.get("/api/session", tags=["api"])
        async def session():
            return {
                "sampling_per_frame": 50,
                "frame_period_ms": 100,
                "protocol_version": 2,
                "frame_type_waveform": 2,
                "transport": "8-bit ADC level, 500 Hz",
                "landmark_codes": {"R": 1, "P": 2, "Q": 3, "S": 4, "T": 5},
                "record_fields": [
                    "sample_index", "t_s", "raw_adc", "filtered_count",
                    "r_event_index", "hr_bpm", "sd_rr_ms", "rmssd_rr_ms",
                    "quality", "alarm", "frame_seq",
                ],
            }

        @app.websocket("/ws")
        async def ws_endpoint(ws: WebSocket):
            await ws.accept()
            try:
                msg = await asyncio.wait_for(ws.receive_json(),
                                             timeout=SUBSCRIBE_TIMEOUT_S)
                subscribed = bool(isinstance(msg, dict)
                                  and msg.get("type") == "subscribe")
            except Exception:
                subscribed = False  # 静默/非法客户端：仅推送一次当前快照
            self._clients.append(ws)

            if not subscribed:
                try:
                    await ws.send_json(self.store.snapshot())
                finally:
                    self._remove_client(ws)
                return

            try:
                while True:
                    await ws.receive_text()  # 仅用于维持连接 / 侦测断开
            except WebSocketDisconnect:
                pass
            except Exception:
                pass
            finally:
                self._remove_client(ws)

        @app.get("/", response_class=HTMLResponse, include_in_schema=False)
        async def index():
            return self._index_html

    # ---- 后台广播 ----
    async def _broadcast_loop(self):
        while True:
            try:
                payload = json.dumps(self.store.snapshot(), ensure_ascii=False)
            except Exception as exc:
                log.warning("snapshot error: %s", exc)
            else:
                dead = []
                for ws in list(self._clients):
                    try:
                        await ws.send_text(payload)
                    except Exception:
                        dead.append(ws)
                for ws in dead:
                    self._remove_client(ws)
            await asyncio.sleep(self.broadcast_delta)

    @asynccontextmanager
    async def _lifespan(self, app: FastAPI):
        """应用启动时拉起广播任务，关闭时取消它。"""
        task = asyncio.create_task(self._broadcast_loop())
        log.info("broadcast loop started (period %.0f ms)",
                 self.broadcast_delta * 1000)
        try:
            yield
        finally:
            task.cancel()
            try:
                await task
            except asyncio.CancelledError:
                pass

    def _remove_client(self, ws: WebSocket) -> None:
        if ws in self._clients:
            self._clients.remove(ws)
