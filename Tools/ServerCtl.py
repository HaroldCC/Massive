"""
@file ServerCtl.py
@brief 本地开发环境一键起服/停服/看日志

通过 xmake 生成的 JSON 文件（xmake.lua -> .pids/payload.json）接收
服务器拓扑、路径等配置，执行对应操作后以 exit code 0/1 返回结果。

用法（由 xmake task 自动调用）：
    python Tools/ServerCtl.py .pids/payload.json
"""

import json
import os
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any


@dataclass
class ServerDef:
    name: str
    bin: str
    port: int
    start_order: int
    config: str
    key_config: str | None = None


@dataclass
class Payload:
    action: str
    project_dir: str
    bin_dir: str
    pids_dir: str
    servers: list[ServerDef]


def _is_win() -> bool:
    return sys.platform == "win32"


def _pidfile(pids_dir: str, name: str) -> Path:
    return Path(pids_dir) / f"{name.lower()}.pid"


def _read_pid(pf: Path) -> int | None:
    if not pf.exists():
        return None
    raw = pf.read_text(encoding="ascii").strip()
    return int(raw) if raw.isdigit() else None


def _bin_path(bin_dir: str, srv: ServerDef) -> Path:
    b = Path(bin_dir) / srv.bin
    if b.suffix == ".exe" and not b.exists():
        alt = b.with_suffix("")
        if alt.exists():
            return alt
    return b


def _server_args(project_dir: str, srv: ServerDef) -> list[str]:
    args = ["--config-path", str(Path(project_dir) / srv.config)]
    if srv.key_config:
        args.extend(["--key-path", str(Path(project_dir) / srv.key_config)])
    return args


# ── up ──

def _is_listening(port: int) -> bool:
    port_str = f":{port}"
    if _is_win():
        try:
            out = subprocess.check_output(
                "cmd /c netstat -ano 2>nul", shell=True, text=True
            )
            for line in out.splitlines():
                if "LISTENING" in line and port_str in line:
                    return True
        except subprocess.CalledProcessError:
            pass
        return False
    else:
        for cmd in [["ss", "-tlnp"], ["netstat", "-tlnp"]]:
            try:
                out = subprocess.check_output(cmd, text=True, stderr=subprocess.DEVNULL)
                if port_str in out:
                    return True
            except (FileNotFoundError, subprocess.CalledProcessError):
                continue
        return False


def _wait_port(port: int, timeout_ms: int) -> bool:
    deadline = time.monotonic() + timeout_ms / 1000
    while time.monotonic() < deadline:
        if _is_listening(port):
            return True
        time.sleep(0.3)
    return _is_listening(port)


def _start(srv: ServerDef, payload: Payload) -> bool:
    bin_path = _bin_path(payload.bin_dir, srv)
    if not bin_path.exists():
        print(f"  XX {srv.name}: cannot find binary -> {bin_path}", file=sys.stderr)
        return False

    args = [str(bin_path)] + _server_args(payload.project_dir, srv)
    pf = _pidfile(payload.pids_dir, srv.name)
    Path(payload.pids_dir).mkdir(parents=True, exist_ok=True)

    if _is_win():
        try:
            proc = subprocess.Popen(
                args,
                creationflags=subprocess.DETACHED_PROCESS,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            # DETACHED_PROCESS 会使进程完全脱离本进程组
            pf.write_text(str(proc.pid), encoding="ascii")
        except OSError as e:
            print(f"  XX {srv.name}: start failed -> {e}", file=sys.stderr)
            return False
    else:
        arg_str = " ".join(f'"{a}"' if " " in a else a for a in args)
        lf = Path(payload.bin_dir) / "logs" / f"{srv.name.lower()}.log"
        lf.parent.mkdir(parents=True, exist_ok=True)
        script = f'"{bin_path}" {arg_str} >> "{lf}" 2>&1 &\necho $! > "{pf}"\n'
        subprocess.run(script, shell=True, check=False)

    print(f"  >> {srv.name:<12s} starting...")
    return True


def cmd_up(payload: Payload) -> int:
    pids_dir = Path(payload.pids_dir)
    if pids_dir.exists():
        shutil.rmtree(pids_dir)

    print(f'{"══════════════════════════════════":>37}')
    print(f'   Massive -- UP')
    print(f'{"══════════════════════════════════":>37}')

    for srv in payload.servers:
        if not _start(srv, payload):
            return 1

    print(f'  {"──── Port Health Check ────":>34}')
    all_ok = True
    deadline = time.monotonic() + 15.0
    for srv in payload.servers:
        remaining = int((deadline - time.monotonic()) * 1000)
        ok = _wait_port(srv.port, max(100, remaining))
        if ok:
            print(f"  ok {srv.name:<12s} :{srv.port} (LISTEN)")
        else:
            print(f"  XX {srv.name:<12s} :{srv.port} (NOT LISTENING)")
            all_ok = False

    if all_ok:
        print(f'{"══════════════════════════════════":>37}')
        print(f'    All ready!')
        print(f'    xmake logs   -- tail logs')
        print(f'    xmake down   -- stop all')
        print(f'{"══════════════════════════════════":>37}')
    else:
        print("  Some services not ready, check logs: xmake logs", file=sys.stderr)
        return 1
    return 0


# ── down ──

def _stop(srv: ServerDef, payload: Payload) -> dict[str, Any]:
    pf = _pidfile(payload.pids_dir, srv.name)
    pid = _read_pid(pf)
    ret: dict[str, Any] = {"name": srv.name, "action": "not-found", "pid": pid, "detail": None}

    if pid is None:
        if _is_win():
            r = subprocess.run(
                ["taskkill", "/F", "/IM", srv.bin],
                capture_output=True, text=True, timeout=10,
            )
            if r.returncode == 0:
                ret["action"] = "fallback"
                ret["detail"] = srv.bin
        else:
            r = subprocess.run(
                ["pkill", "-f", srv.name],
                capture_output=True, timeout=10,
            )
            if r.returncode == 0:
                ret["action"] = "fallback"
                ret["detail"] = srv.name
        return ret

    if _is_win():
        r = subprocess.run(
            ["tasklist", "/FI", f'"PID eq {pid}"', "/NH"],
            capture_output=True, text=True, timeout=10,
        )
        if r.returncode != 0 or not r.stdout or "No " in r.stdout:
            ret["action"] = "dead"
            pf.unlink(missing_ok=True)
            return ret

        r = subprocess.run(
            ["taskkill", "/PID", str(pid)],
            capture_output=True, timeout=10,
        )
        if r.returncode == 0:
            time.sleep(2.0)
            r2 = subprocess.run(
                ["tasklist", "/FI", f'"PID eq {pid}"', "/NH"],
                capture_output=True, text=True, timeout=10,
            )
            st = r2.stdout or ""
            if r2.returncode == 0 and st and "No " not in st:
                subprocess.run(
                    ["taskkill", "/F", "/PID", str(pid)],
                    capture_output=True, timeout=10,
                )
                ret["action"] = "force"
            else:
                ret["action"] = "graceful"
        else:
            ret["action"] = "pid-not-found"
            subprocess.run(
                ["taskkill", "/F", "/IM", srv.bin],
                capture_output=True, timeout=10,
            )
            ret["action"] = "fallback"
            ret["detail"] = srv.bin
    else:
        r = subprocess.run(["kill", str(pid)], capture_output=True, timeout=10)
        if r.returncode == 0:
            time.sleep(0.5)
            r2 = subprocess.run(["kill", "-0", str(pid)], capture_output=True, timeout=5)
            if r2.returncode == 0:
                subprocess.run(["kill", "-9", str(pid)], capture_output=True, timeout=5)
                ret["action"] = "force"
            else:
                ret["action"] = "graceful"
        else:
            ret["action"] = "dead"

    pf.unlink(missing_ok=True)
    return ret


def cmd_down(payload: Payload) -> int:
    print(f'{"══════════════════════════════════":>37}')
    print(f'   Massive -- DOWN')
    print(f'{"══════════════════════════════════":>37}')

    nkilled = 0
    for srv in reversed(payload.servers):
        r = _stop(srv, payload)
        if r["action"] != "not-found":
            nkilled += 1
        pid_str = f"(PID {r['pid']})" if r["pid"] else ""
        a = r["action"]
        if a == "graceful":
            print(f"  ok {srv.name} graceful exit {pid_str}")
        elif a == "force":
            print(f"  XX {srv.name} force killed {pid_str}")
        elif a == "dead":
            print(f"  ?? {srv.name} already dead {pid_str}")
        elif a == "fallback":
            print(f"  XX {srv.name} killed (fallback: {r['detail']})")
        elif a == "pid-not-found":
            print(f"  ?? {srv.name} no perm, fallback used")
        else:
            print(f"  ?? {srv.name} (not started)")
        time.sleep(0.3)

    shutil.rmtree(payload.pids_dir, ignore_errors=True)
    if nkilled > 0:
        print(f"  Stopped {nkilled} services.")
    else:
        print("  No running services.")
    return 0


# ── logs ──

def cmd_logs(payload: Payload) -> int:
    logdir = Path(payload.bin_dir) / "logs"
    if not logdir.is_dir():
        print(f"Log dir not found: {logdir}", file=sys.stderr)
        print("Run xmake up first.", file=sys.stderr)
        return 1

    logfiles: list[dict[str, Any]] = []
    for srv in payload.servers:
        lf = logdir / f"{srv.name.lower()}.log"
        if lf.exists():
            logfiles.append({"name": srv.name, "path": str(lf)})

    if not logfiles:
        print("No log files found.", file=sys.stderr)
        return 1

    print(f"Tailing {len(logfiles)} log files (Ctrl+C per window to exit)")

    if _is_win():
        import ctypes
        for lf in logfiles:
            title = f"Massive: {lf['name']}"
            subprocess.Popen(
                ["powershell", "-NoExit", "-Command",
                 f"Get-Content -Wait -Tail 20 '{lf['path']}'"],
                creationflags=subprocess.CREATE_NEW_CONSOLE,
            )
        # 阻塞等用户回车，否则 xmake task 一闪而过
        input("Press Enter to stop tailing...")
        return 0
    else:
        args = ["tail"]
        for lf in logfiles:
            args.extend(["-n", "20", lf["path"]])
        subprocess.run(args)
        return 0


# ── entry ──

def main() -> None:
    if len(sys.argv) >= 2:
        path = Path(sys.argv[1])
        if path.exists():
            data = json.loads(path.read_text(encoding="utf-8"))
        else:
            print(f"Payload file not found: {path}", file=sys.stderr)
            sys.exit(1)
    else:
        data = json.load(sys.stdin)
    p = Payload(
        action=data["action"],
        project_dir=data["project_dir"],
        bin_dir=data["bin_dir"],
        pids_dir=data["pids_dir"],
        servers=[ServerDef(**s) for s in data["servers"]],
    )

    if p.action == "up":
        sys.exit(cmd_up(p))
    elif p.action == "down":
        sys.exit(cmd_down(p))
    elif p.action == "logs":
        sys.exit(cmd_logs(p))
    else:
        print(f"Unknown action: {p.action}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
