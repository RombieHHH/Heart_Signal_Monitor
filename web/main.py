# -*- coding: utf-8 -*-
"""
ECG 上位机后端入口。

用法：
    python main.py [--port 串口或auto] [--baud 波特率] [--host 0.0.0.0]
                    [--http-port 8000] [--record-dir 记录目录] [--no-record]
                    [-v]

启动后：
    - 后台线程读取蓝牙串口并解析数据帧；
    - CSV 记录（默认开启，写入 ./recordings/）；
    - FastAPI 服务提供 /api/* 与 /ws，并在 / 提供 web/index.html 前端页面。
"""

import argparse
import logging
import os
import signal

import uvicorn

from ecg import datastore
from ecg.reader import SerialReader
from ecg.recorder import CsvRecorder
from ecg.server import WebServer

# 项目根目录下建立记录目录
BASE_DIR = os.path.dirname(os.path.abspath(__file__))


def parse_args(argv=None):
    p = argparse.ArgumentParser(description="ECG 上位机后端（Python）")
    p.add_argument("--port", default="auto", help="蓝牙虚拟串口；auto 按 HSM(0023:00:0032AA) 自动识别，或给定如 COM29")
    p.add_argument("--baud", type=int, default=921600, help="串口波特率（HSM 蓝牙模块为 921600）")
    p.add_argument("--host", default="0.0.0.0", help="HTTP 监听地址")
    p.add_argument("--http-port", type=int, default=8000, help="HTTP/WebSocket 端口")
    p.add_argument("--record-dir", default=None, help="CSV 记录目录（默认 <项目>/recordings）")
    p.add_argument("--no-record", action="store_true", help="关闭 CSV 记录")
    p.add_argument("-v", "--verbose", action="store_true", help="输出调试日志")
    return p.parse_args(argv)


def setup_logging(verbose=False):
    level = logging.DEBUG if verbose else logging.INFO
    logging.basicConfig(
        level=level,
        format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
    )


def main(argv=None):
    args = parse_args(argv)
    setup_logging(args.verbose)
    log = logging.getLogger("ecg.main")

    record_dir = args.record_dir or os.path.join(BASE_DIR, "recordings")
    store = datastore.DataStore(window_samples=2000)  # 4 s @ 500 Hz

    recorder = None
    if not args.no_record:
        recorder = CsvRecorder(record_dir)

    reader = SerialReader(store=store, recorder=recorder,
                          port=args.port, baudrate=args.baud)
    reader.start()

    # web/index.html 是随仓库提供的前端页面，无需再通过命令行参数指定目录
    web = WebServer(store=store,
                    index_file=os.path.join(BASE_DIR, "web", "index.html"))

    if recorder is not None:
        log.info("本会话 CSV 记录：%s", recorder.files[0])

    # Ctrl+C 优雅退出
    def _shutdown(*_):
        log.info("shutting down ...")
        reader.stop()
    signal.signal(signal.SIGINT, _shutdown)
    signal.signal(signal.SIGTERM, _shutdown)

    try:
        uvicorn.run(web.app, host=args.host, port=args.http_port, log_level="info")
    except KeyboardInterrupt:
        pass
    finally:
        reader.stop()
        reader.join(timeout=2.0)
        if recorder is not None:
            recorder.close()
    log.info("exit")


if __name__ == "__main__":
    main()
