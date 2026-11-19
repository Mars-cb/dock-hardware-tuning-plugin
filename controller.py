#!/usr/bin/python3 -I
"""Read supported hardware settings and apply one validated sysfs change."""
import json
import os
import grp
import pwd
from pathlib import Path
import re
import subprocess
import sys
import time

SYS = Path("/sys")
CPU = SYS / "devices/system/cpu/cpufreq"
ASPM = SYS / "module/pcie_aspm/parameters/policy"
GPU_LEVELS = ["auto", "low", "high"]
FAN_LEVELS = ["auto", "70", "85", "100"]
FAN_TOLERANCE_PERCENT = 3
SERVICE = "io.github.DockHardwareTuning"
OBJECT = "/io/github/DockHardwareTuning"
LOGIN = "org.freedesktop.login1"


def read(path):
    try:
        return path.read_text(encoding="ascii").strip()
    except (OSError, UnicodeError):
        return ""


def write(path, value):
    with path.open("w", encoding="ascii") as stream:
        stream.write(value + "\n")


def number(path):
    try:
        return int(read(path))
    except ValueError:
        return None


def gpus():
    devices = {}
    for card in sorted((SYS / "class/drm").glob("card*")):
        if not re.fullmatch(r"card\d+", card.name):
            continue
        device = (card / "device").resolve()
        if not re.fullmatch(r"[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-7]", device.name):
            continue
        if read(device / "vendor") != "0x1002" or (device / "driver").resolve().name != "amdgpu":
            continue
        hwmon = next((p for p in sorted((device / "hwmon").glob("hwmon*"))
                      if read(p / "name") == "amdgpu"), None)
        devices[device.name] = (device, hwmon)
    return devices


def fan_range(hwmon):
    low = number(hwmon / "pwm1_min")
    high = number(hwmon / "pwm1_max")
    if low is not None and high is not None and 0 <= low < high <= 255:
        return low, high
    return None


def fan_value(hwmon):
    mode = read(hwmon / "pwm1_enable")
    if mode == "2":
        return "auto"
    duty = number(hwmon / "pwm1")
    limits = fan_range(hwmon)
    if mode == "1" and duty is not None and limits:
        return str(round(100 * duty / limits[1]))
    return "mode=" + mode if mode else ""


def disks():
    return {path.name: path for path in sorted((SYS / "block").glob("*"))
            if re.fullmatch(r"(?:sd[a-z]+|nvme[0-9]+n[0-9]+)", path.name)
            and "/virtual/" not in str(path.resolve())}


def disk_telemetry():
    mounted = {parts[4] for line in read(Path("/proc/self/mountinfo")).splitlines()
               for parts in [line.split()] if len(parts) >= 5}
    volumes = {}
    for mount in ("/", "/data"):
        if mount not in mounted:
            continue
        try:
            stats = os.statvfs(mount)
        except OSError:
            continue
        volumes[mount] = {"total": stats.f_blocks * stats.f_frsize,
                          "used": (stats.f_blocks - stats.f_bfree) * stats.f_frsize}
    result = []
    for name, device in disks().items():
        stats = read(device / "stat").split()
        sensors = sorted(device.glob("device/hwmon/hwmon*"))
        for parent in device.resolve().parents:
            if parent.name.startswith("nvme"):
                sensors.extend(sorted(parent.glob("hwmon/hwmon*")))
        temperature = next((value for sensor in sensors
                            for value in [number(sensor / "temp1_input")]
                            if value is not None), None)
        if len(stats) >= 7:
            result.append({"id": name, "read": int(stats[2]) * 512,
                           "write": int(stats[6]) * 512, "temperature": temperature})
    return {"disks": result, "volumes": volumes, "time": time.monotonic()}


def controls():
    result = {}
    policies = sorted(CPU.glob("policy*"))
    if policies:
        paths = [p / "scaling_governor" for p in policies]
        modes = set(read(policies[0] / "scaling_available_governors").split())
        for policy in policies[1:]:
            modes.intersection_update(read(policy / "scaling_available_governors").split())
        if modes and all(read(path) for path in paths):
            result["governor"] = (paths, sorted(modes), None)
    if read(CPU / "boost") in ("0", "1"):
        result["boost"] = ([CPU / "boost"], ["1", "0"], None)
    modes = [s.strip("[]") for s in read(ASPM).split()]
    if modes:
        result["aspm"] = ([ASPM], modes, None)
    for pci, (device, hwmon) in gpus().items():
        level = device / "power_dpm_force_performance_level"
        if read(level):
            result["gpu:" + pci] = ([level], GPU_LEVELS, None)
        if hwmon and fan_range(hwmon) and fan_value(hwmon):
            result["fan:" + pci] = ([], FAN_LEVELS, hwmon)
    for name, device in disks().items():
        scheduler = device / "queue/scheduler"
        choices = [value.strip("[]") for value in read(scheduler).split()]
        if len(choices) > 1:
            result["scheduler:" + name] = ([scheduler], choices, None)
    return result


def current(key, paths, hwmon):
    if hwmon:
        return fan_value(hwmon)
    values = {read(path) for path in paths}
    if key == "aspm" or key.startswith("scheduler:"):
        return next((s[1:-1] for s in next(iter(values)).split()
                     if s.startswith("[") and s.endswith("]")), "")
    return next(iter(values)) if len(values) == 1 else ""


def active_clock(device, clock):
    match = re.search(r"([0-9]+)Mhz\s+\*", read(device / clock), re.IGNORECASE)
    return int(match.group(1)) if match else None


def utilization():
    stats = read(Path("/proc/stat")).splitlines()
    fields = stats[0].split()[1:9] if stats else []
    ticks = [int(value) for value in fields]
    memory = {}
    for line in read(Path("/proc/meminfo")).splitlines():
        key, _, value = line.partition(":")
        if key in ("MemTotal", "MemAvailable"):
            memory[key] = int(value.split()[0]) * 1024
    clocks = [number(policy / "scaling_cur_freq") for policy in CPU.glob("policy*")]
    clocks = [clock for clock in clocks if clock is not None]
    return {"totalTicks": sum(ticks) if len(ticks) >= 5 else None,
            "idleTicks": sum(ticks[3:5]) if len(ticks) >= 5 else None,
            "memoryTotal": memory.get("MemTotal"),
            "memoryAvailable": memory.get("MemAvailable"),
            "cpuPeakKHz": max(clocks) if clocks else None}


def temperature_risk(value, critical):
    if value is None:
        return 0
    threshold = critical if critical is not None and 50000 <= critical <= 120000 else 95000
    return 2 if value >= threshold else 1 if value >= threshold - 10000 else 0


def telemetry():
    cpu = {}
    cpu_risk = 0
    for hwmon in sorted((SYS / "class/hwmon").glob("hwmon*")):
        name = read(hwmon / "name")
        if name not in ("coretemp", "k10temp", "zenpower"):
            continue
        for path in sorted(hwmon.glob("temp*_input")):
            label = read(path.with_name(path.name.replace("_input", "_label")))
            if label.startswith("Package id") or label in ("Tdie", "Tctl"):
                value = number(path)
                if value is not None:
                    cpu[hwmon.name + " " + label] = value
                    critical = number(path.with_name(path.name.replace("_input", "_crit")))
                    cpu_risk = max(cpu_risk, temperature_risk(value, critical))
        if not any(key.startswith(hwmon.name + " ") for key in cpu):
            value = number(hwmon / "temp1_input")
            if value is not None:
                cpu[hwmon.name + " " + name] = value
                cpu_risk = max(cpu_risk, temperature_risk(value, number(hwmon / "temp1_crit")))
    gpu = []
    for pci, (device, hwmon) in gpus().items():
        temperature = number(hwmon / "temp1_input") if hwmon else None
        critical = number(hwmon / "temp1_crit") if hwmon else None
        gpu.append({"id": pci,
                    "temperature": temperature,
                    "risk": temperature_risk(temperature, critical),
                    "rpm": number(hwmon / "fan1_input") if hwmon else None,
                    "fan": fan_value(hwmon) if hwmon else "",
                    "busy": number(device / "gpu_busy_percent"),
                    "memoryBusy": number(device / "mem_busy_percent"),
                    "vramUsed": number(device / "mem_info_vram_used"),
                    "vramTotal": number(device / "mem_info_vram_total"),
                    "coreMHz": active_clock(device, "pp_dpm_sclk"),
                    "memoryMHz": active_clock(device, "pp_dpm_mclk"),
                    "power": number(hwmon / "power1_average") if hwmon else None})
    return {"cpu": cpu, "cpuRisk": cpu_risk, "gpu": gpu, **utilization(), **disk_telemetry()}


def apply_fan(hwmon, value):
    enable = hwmon / "pwm1_enable"
    if value == "auto":
        write(enable, "2")
        if read(enable) != "2":
            raise ValueError("驱动未确认自动风扇模式")
        return
    pwm = hwmon / "pwm1"
    limits = fan_range(hwmon)
    if not limits:
        raise ValueError("风扇占空比范围不可用")
    low, high = limits
    target = max(low, round(high * int(value) / 100))
    try:
        write(enable, "1")
        write(pwm, str(target))
        deadline = time.monotonic() + 1.2
        while True:
            mode = read(enable)
            actual = number(pwm)
            if (mode == "1" and actual is not None and low <= actual <= high
                    and abs(actual - target) * 100 <= FAN_TOLERANCE_PERCENT * high):
                return
            if time.monotonic() >= deadline:
                raise ValueError("驱动读回模式 {}、PWM {}，目标 PWM {}".format(mode, actual, target))
            time.sleep(0.1)
    except (OSError, ValueError) as error:
        try:
            write(enable, "2")
            if read(enable) != "2":
                raise ValueError("未确认自动模式")
        except (OSError, ValueError) as rollback:
            raise ValueError("风扇控制失败，恢复自动模式也失败：{}".format(rollback))
        raise ValueError("风扇控制失败，已恢复自动：{}".format(error))


def apply(key, value, paths, hwmon):
    if hwmon:
        apply_fan(hwmon, value)
        return
    changed = []
    try:
        for path in paths:
            previous = current(key, [path], None)
            if not previous:
                raise ValueError("无法读取原值")
            if previous != value:
                write(path, value)
                changed.append((path, previous))
            if current(key, [path], None) != value:
                raise ValueError("驱动未确认设置")
    except (OSError, ValueError):
        failed = []
        for path, previous in reversed(changed):
            try:
                write(path, previous)
                if current(key, [path], None) != previous:
                    failed.append(path.name)
            except OSError:
                failed.append(path.name)
        if failed:
            raise ValueError("设置失败，部分原值未恢复：" + "、".join(failed))
        raise


SOURCE_HELPER = "/usr/libexec/dde-dock-input-source/dde-dock-input-source-helper"
DDCUTIL = "/usr/bin/ddcutil"
MONITOR_FEATURES = {"brightness": "10", "contrast": "12"}


def run_command(*args, timeout):
    try:
        result = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                encoding="utf-8", errors="replace", timeout=timeout,
                                env={**os.environ, "LC_ALL": "C"})
    except subprocess.TimeoutExpired:
        raise ValueError("显示器 DDC/CI 操作超时")
    if result.returncode:
        message = result.stderr.strip() or result.stdout.strip()
        raise ValueError(message[-400:] or "显示器操作失败")
    return result.stdout


def source_helper(*args):
    response = json.loads(run_command(SOURCE_HELPER, *args, timeout=45))
    if response.get("ok") is not True:
        raise ValueError(response.get("error") or "显示器检测失败")
    return response


def monitor_identity(monitor):
    connector, bus, edid = (monitor.get(field) for field in ("id", "bus", "edid"))
    if (not isinstance(connector, str)
            or not re.fullmatch(r"card[0-9]+-[A-Za-z0-9-]+", connector)
            or type(bus) is not int or bus < 0 or bus > 65535
            or not isinstance(edid, str) or not re.fullmatch(r"[0-9a-fA-F]{256}", edid)):
        return None
    return connector, str(bus), edid.lower()


def monitor_levels(bus, code):
    output = run_command(DDCUTIL, "--bus", bus, "--terse", "getvcp", code, timeout=5)
    match = re.search(r"^VCP " + code + r" C (\d+) (\d+)", output, re.MULTILINE)
    if not match:
        return None
    actual, maximum = map(int, match.groups())
    return (actual, maximum) if 0 <= actual <= maximum and maximum > 0 else None


def monitor_controls():
    rows = []
    for monitor in source_helper("scan").get("monitors", []):
        identity = monitor_identity(monitor)
        name = "{} · {}".format(monitor.get("name") or "显示器",
                               monitor.get("connector") or "未知接口")
        sources = [str(v) for v in monitor.get("sources", [])
                   if type(v) is int and 1 <= v <= 255]
        if not identity:
            rows.append({"title": name, "detail": monitor.get("error") or "无法确认 DDC/CI 连接"})
            continue
        connector, bus, edid = identity
        if monitor.get("controllable") is True and sources:
            rows.append({"id": ":".join(("monitor", "input", connector, bus, edid)),
                         "title": name + " · 输入源", "options": sources,
                         "current": str(monitor.get("current", ""))})
        else:
            rows.append({"title": name + " · 输入源",
                         "detail": monitor.get("error") or "显示器未报告可选输入源"})
        for feature, code in MONITOR_FEATURES.items():
            try:
                levels = monitor_levels(bus, code)
            except (OSError, ValueError):
                continue
            if not levels:
                continue
            actual, maximum = levels
            steps = {str(round(maximum * percent / 100)) for percent in range(10, 101, 10)}
            options = sorted(steps | {str(actual)}, key=int)
            rows.append({"id": ":".join(("monitor", feature, connector, bus, edid)),
                         "title": name + (" · 亮度" if feature == "brightness" else " · 对比度"),
                         "options": options, "current": str(actual), "maximum": maximum})
    return rows


def monitor_inputs(keys):
    readings = {}
    for key in keys:
        parts = key.split(":")
        if len(parts) != 5 or parts[:2] != ["monitor", "input"]:
            continue
        _, _, connector, bus, edid = parts
        if not re.fullmatch(r"[0-9]{1,5}", bus):
            continue
        if monitor_identity({"id": connector, "bus": int(bus), "edid": edid}) != (connector, bus, edid):
            continue
        path = SYS / "class/drm" / connector
        try:
            if (path / "edid").read_bytes()[:128].hex() != edid:
                continue
        except OSError:
            continue
        # A DP link change can recreate the AUX adapter with another bus number.
        candidates = [entry.name for entry in path.glob("i2c-*")]
        if not candidates:
            candidates = [(path / "ddc").resolve().name, "i2c-" + bus]
        for candidate in dict.fromkeys(candidates):
            match = re.fullmatch(r"i2c-([0-9]+)", candidate)
            if not match:
                continue
            try:
                output = run_command(DDCUTIL, "--bus", match[1], "--terse", "getvcp", "60", timeout=3)
            except (OSError, ValueError):
                continue
            match = re.search(r"^VCP 60 SNC x([0-9a-f]{1,2})\s*$", output, re.MULTILINE | re.IGNORECASE)
            if match and int(match[1], 16):
                readings[key] = str(int(match[1], 16))
                break
    return readings


def set_monitor(key, value):
    parts = key.split(":")
    if len(parts) != 5 or parts[0] != "monitor" or not re.fullmatch(r"[0-9]{1,5}", value):
        raise ValueError("无效的显示器设置")
    _, feature, connector, bus, edid = parts
    if not re.fullmatch(r"[0-9]{1,5}", bus):
        raise ValueError("无效的 I²C 总线")
    identity = monitor_identity({"id": connector, "bus": int(bus), "edid": edid})
    if identity != (connector, bus, edid) or feature not in {*MONITOR_FEATURES, "input"}:
        raise ValueError("无效的显示器目标")
    if feature == "input":
        if not 1 <= int(value) <= 255:
            raise ValueError("无效的输入源")
        source_helper("set", bus, edid, value)
        return
    connector_path = SYS / "class/drm" / connector
    if feature == "brightness":
        if not 1 <= int(value) <= 65535:
            raise ValueError("无效的亮度值")
        bus_path = (SYS / "bus/i2c/devices" / ("i2c-" + bus)).resolve(strict=True)
        if (bus_path.parent == connector_path.resolve(strict=True)
                or bus_path == (connector_path / "ddc").resolve()):
            if (connector_path / "edid").read_bytes()[:128].hex() != edid:
                raise ValueError("显示器身份已变化，请刷新后重试")
            run_command(DDCUTIL, "--bus", bus, "--verify", "setvcp", "10", value, timeout=8)
            return
    matches = [monitor for monitor in source_helper("scan").get("monitors", [])
               if monitor_identity(monitor) == identity]
    if len(matches) != 1:
        raise ValueError("显示器连接已变化，请刷新后重试")
    code = MONITOR_FEATURES[feature]
    levels = monitor_levels(bus, code)
    if not levels or not 1 <= int(value) <= levels[1]:
        raise ValueError("设置超出显示器支持的范围")
    actual_edid = (SYS / "class/drm" / connector / "edid").read_bytes()[:128].hex()
    if actual_edid != edid:
        raise ValueError("显示器身份已变化，请刷新后重试")
    run_command(DDCUTIL, "--bus", bus, "setvcp", code, value, timeout=8)
    levels = monitor_levels(bus, code)
    if not levels or levels[0] != int(value):
        raise ValueError("指令已发送，但读回值不符")


def set_hardware(key, value):
    available = controls()
    if key not in available:
        raise ValueError("此设置在当前硬件上不可用")
    paths, options, hwmon = available[key]
    if value not in options:
        raise ValueError("当前驱动不支持这个选项")
    apply(key, value, paths, hwmon)
    return current(key, paths, hwmon)


def active_desktop_admin(bus, sender):
    import dbus

    daemon = bus.get_object("org.freedesktop.DBus", "/org/freedesktop/DBus")
    uid = int(daemon.GetConnectionUnixUser(sender, dbus_interface="org.freedesktop.DBus"))
    user = pwd.getpwuid(uid)
    if grp.getgrnam("sudo").gr_gid not in os.getgrouplist(user.pw_name, user.pw_gid):
        return False
    login = bus.get_object(LOGIN, "/org/freedesktop/login1")
    sessions = login.ListSessions(dbus_interface=LOGIN + ".Manager", timeout=3)
    for _, owner, _, seat, path in sessions:
        if int(owner) != uid or not seat:
            continue
        session = bus.get_object(LOGIN, path)
        properties = session.GetAll(LOGIN + ".Session",
                                    dbus_interface="org.freedesktop.DBus.Properties", timeout=3)
        if (properties.get("Active") and not properties.get("Remote")
                and properties.get("Type") in ("x11", "wayland")):
            return True
    return False


def serve():
    import dbus
    import dbus.service
    from dbus.mainloop.glib import DBusGMainLoop
    from gi.repository import GLib

    if os.geteuid() != 0:
        raise ValueError("硬件服务必须由系统启动")
    DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()

    class HardwareService(dbus.service.Object):
        @dbus.service.method(SERVICE, in_signature="ss", out_signature="s", sender_keyword="sender")
        def Apply(self, key, value, sender=None):
            try:
                allowed = sender and active_desktop_admin(bus, sender)
            except (dbus.DBusException, KeyError, OSError):
                allowed = False
            if not allowed:
                raise dbus.DBusException("仅当前活动的本地桌面管理员可调控硬件",
                                         name=SERVICE + ".AccessDenied")
            try:
                result = set_hardware(str(key), str(value))
                if key.startswith("fan:"):
                    print("GPU fan {}: requested {}, read back {}".format(key, value, result),
                          file=sys.stderr, flush=True)
                return result
            except (OSError, ValueError) as error:
                print("Hardware apply failed: {}".format(error), file=sys.stderr, flush=True)
                raise dbus.DBusException(str(error), name=SERVICE + ".ApplyFailed")

    name = dbus.service.BusName(SERVICE, bus=bus, do_not_queue=True)
    server = HardwareService(name, OBJECT)
    try:
        GLib.MainLoop().run()
    finally:
        server.remove_from_connection()


def request_hardware(key, value):
    import dbus

    try:
        bus = dbus.SystemBus()
        proxy = bus.get_object(SERVICE, OBJECT, introspect=False)
        return str(proxy.Apply(key, value, dbus_interface=SERVICE, timeout=15))
    except dbus.DBusException as error:
        raise ValueError(error.get_dbus_message() or "无法连接硬件系统服务")


def main(args):
    if args == ["service"]:
        serve()
        return
    if len(args) == 3 and args[0] == "request":
        result = request_hardware(args[1], args[2])
        if result:
            print(result)
        return
    if args and args[0] == "monitor-inputs":
        print(json.dumps(monitor_inputs(args[1:])))
        return
    if args == ["monitor-list"]:
        if not Path(SOURCE_HELPER).is_file():
            raise ValueError("显示器页需要安装 dde-dock-input-source")
        print(json.dumps(monitor_controls()))
        return
    if len(args) == 3 and args[0] == "monitor-set":
        set_monitor(args[1], args[2])
        return
    if args == ["telemetry"]:
        print(json.dumps(telemetry()))
        return
    if args == ["list"]:
        print(json.dumps([{"id": key, "options": options,
                           "current": current(key, paths, hwmon)}
                          for key, (paths, options, hwmon) in controls().items()]))
        return
    if len(args) != 3 or args[0] != "set":
        raise ValueError("用法：list | telemetry | set 项目 值")
    if os.geteuid() != 0:
        raise ValueError("写入设置需要管理员授权")
    set_hardware(*args[1:])


if __name__ == "__main__":
    try:
        main(sys.argv[1:])
    except (OSError, ValueError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
