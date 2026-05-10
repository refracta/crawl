import datetime
import gc
import logging
import os
import signal
import time

from tornado.ioloop import IOLoop

from webtiles import auth, config, process_handler, ws_handler

try:
    import asyncio
except ImportError:
    asyncio = None

try:
    import tracemalloc
except ImportError:
    tracemalloc = None


_started = False
_start_time = None


def _safe_len(value):
    try:
        return len(value)
    except Exception:
        return None


def _rss_kb():
    try:
        with open("/proc/self/status") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
    except Exception:
        return None
    return None


def _fd_count():
    try:
        return len(os.listdir("/proc/self/fd"))
    except Exception:
        return None


def _receiver_count(processes):
    total = 0
    for process in processes:
        receivers = getattr(process, "_receivers", ())
        total += len(receivers)
    return total


def _lobby_socket_count(sockets):
    total = 0
    for socket in list(sockets):
        try:
            if socket.is_in_lobby():
                total += 1
        except Exception:
            pass
    return total


def _asyncio_counts():
    if asyncio is None:
        return None, None
    try:
        loop = asyncio.get_event_loop()
    except Exception:
        return None, None

    ready = getattr(loop, "_ready", None)
    scheduled = getattr(loop, "_scheduled", None)
    ready_count = len(ready) if ready is not None else None
    scheduled_count = len(scheduled) if scheduled is not None else None
    return ready_count, scheduled_count


def _metrics_snapshot(reason, expected_at=None):
    now = time.time()
    processes = list(process_handler.processes.values())
    sockets = list(ws_handler.sockets)
    ready_count, scheduled_count = _asyncio_counts()
    lag_ms = None
    if expected_at is not None:
        lag_ms = max(0.0, (now - expected_at) * 1000.0)
    uptime_s = None
    if _start_time is not None:
        uptime_s = int(now - _start_time)

    metrics = {
        "reason": reason,
        "pid": os.getpid(),
        "uptime_s": uptime_s,
        "rss_kb": _rss_kb(),
        "fd_count": _fd_count(),
        "sockets": len(sockets),
        "lobby_sockets": _lobby_socket_count(sockets),
        "processes": len(processes),
        "receivers": _receiver_count(processes),
        "game_lobby_cache": _safe_len(ws_handler.game_lobby_cache),
        "login_tokens": _safe_len(auth.login_tokens),
        "ioloop_lag_ms": None if lag_ms is None else round(lag_ms, 1),
        "asyncio_ready": ready_count,
        "asyncio_scheduled": scheduled_count,
        "gc_count": gc.get_count(),
        "tracemalloc": bool(tracemalloc and tracemalloc.is_tracing()),
    }
    return metrics


def _format_value(value):
    if isinstance(value, tuple):
        return ",".join(str(v) for v in value)
    return str(value)


def log_metrics(reason, expected_at=None):
    metrics = _metrics_snapshot(reason, expected_at)
    logging.info("WEBTILES_METRICS %s",
                 " ".join("%s=%s" % (key, _format_value(value))
                          for key, value in sorted(metrics.items())))


def _schedule_next():
    interval = config.get('webtiles_metrics_interval', 0)
    if not interval:
        return

    expected_at = time.time() + interval
    IOLoop.current().add_timeout(expected_at, lambda: _metrics_tick(expected_at))


def _metrics_tick(expected_at):
    try:
        log_metrics("periodic", expected_at)
    except Exception:
        logging.warning("Failed to log WebTiles metrics.", exc_info=True)
    finally:
        _schedule_next()


def _snapshot_dir():
    directory = config.get('webtiles_metrics_snapshot_dir')
    if directory:
        return directory

    pidfile = config.get('pidfile')
    if pidfile:
        return os.path.dirname(pidfile)

    logging_config = config.get('logging_config') or {}
    logfile = logging_config.get('filename')
    if logfile:
        return os.path.dirname(logfile)

    return "/tmp"


def dump_tracemalloc_snapshot():
    if not tracemalloc or not tracemalloc.is_tracing():
        logging.warning("WEBTILES_TRACEMALLOC inactive")
        return

    directory = _snapshot_dir()
    timestamp = datetime.datetime.utcnow().strftime("%Y%m%dT%H%M%SZ")
    path = os.path.join(directory, "webtiles-tracemalloc-%s-%s.snapshot" %
                        (os.getpid(), timestamp))

    snapshot = tracemalloc.take_snapshot()
    snapshot.dump(path)
    logging.warning("WEBTILES_TRACEMALLOC_SNAPSHOT path=%s", path)

    for stat in snapshot.statistics('lineno')[:20]:
        logging.info("WEBTILES_TRACEMALLOC_TOP %s", stat)


def dump_state():
    try:
        log_metrics("signal")
        dump_tracemalloc_snapshot()
    except Exception:
        logging.warning("Failed to dump WebTiles metrics state.", exc_info=True)


def _signal_from_config():
    signal_name = config.get('webtiles_metrics_signal', 'SIGUSR2')
    if not signal_name:
        return None
    if isinstance(signal_name, int):
        return signal_name
    return getattr(signal, signal_name, None)


def _install_signal_handler():
    if asyncio is None:
        return
    sig = _signal_from_config()
    if sig is None:
        logging.warning("Unknown webtiles_metrics_signal value: %s",
                        config.get('webtiles_metrics_signal'))
        return

    try:
        asyncio.get_event_loop().add_signal_handler(sig, dump_state)
        logging.info("WebTiles metrics dump signal installed: %s",
                     config.get('webtiles_metrics_signal', 'SIGUSR2'))
    except Exception:
        logging.warning("Failed to install WebTiles metrics signal handler.",
                        exc_info=True)


def _start_tracemalloc():
    if not config.get('webtiles_metrics_tracemalloc'):
        return
    if tracemalloc is None:
        logging.warning("tracemalloc is unavailable.")
        return
    if tracemalloc.is_tracing():
        return
    frames = config.get('webtiles_metrics_tracemalloc_frames', 15)
    tracemalloc.start(frames)
    logging.info("tracemalloc started with %s frames.", frames)


def start():
    global _started, _start_time
    if _started:
        return

    _started = True
    _start_time = time.time()
    _start_tracemalloc()
    _install_signal_handler()
    log_metrics("startup")
    _schedule_next()
