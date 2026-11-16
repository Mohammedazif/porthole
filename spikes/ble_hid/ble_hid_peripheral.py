#!/usr/bin/env python3
"""Spikes S3/S5: make this Linux machine a Bluetooth LE mouse + keyboard for an iPhone.

Uses BlueZ over D-Bus to publish a HID-over-GATT (HOGP) device called "Porthole".
The HID report descriptor is read straight from src/hid/hid_reports.c, so the spike
tests exactly the bytes the real app will use.

Commands are read line by line from a FIFO (default $XDG_RUNTIME_DIR/porthole-hid.fifo):

  move DX DY [STEP]   relative pointer move, split into steps of at most STEP (default 20)
  path TOKEN...       one continuous movement; each token is AXIS STEP '*' COUNT,
                      e.g. "path x32*5 x4*2 y-16*3" (no pauses between reports)
  anchor              push the pointer into the top-left corner
  click [BTN]         press + release mouse button 1..8 (default 1 = left)
  down BTN | up BTN   hold / release a mouse button
  drag DX DY [STEP]   hold the left button, move, release
  scroll N            vertical wheel, positive = up
  pan N               horizontal scroll
  key USAGE [MODS]    press + release a keyboard usage (hex ok); MODS = modifier bits
  type TEXT...        type ASCII text (US layout)
  consumer USAGE      press + release a consumer usage, e.g. 0x223 (AC Home)
  interval MS         delay between reports (default 15)
  sleep MS            wait
  readvertise         unregister and re-register the advertisement (for btmon captures)
  reregister          remove and re-add the HID service so the iPhone subscribes again
                      (also done automatically when it reconnects without subscribing)
  status              print connection state
  quit                stop

Example:  echo "anchor" > $XDG_RUNTIME_DIR/porthole-hid.fifo
"""

import argparse
import math
import os
import re
import signal
import struct
import sys
import threading
import time
from pathlib import Path

import dbus
import dbus.exceptions
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

BLUEZ = "org.bluez"
ADAPTER_IFACE = "org.bluez.Adapter1"
DEVICE_IFACE = "org.bluez.Device1"
GATT_MANAGER_IFACE = "org.bluez.GattManager1"
GATT_SERVICE_IFACE = "org.bluez.GattService1"
GATT_CHRC_IFACE = "org.bluez.GattCharacteristic1"
GATT_DESC_IFACE = "org.bluez.GattDescriptor1"
LE_ADV_MANAGER_IFACE = "org.bluez.LEAdvertisingManager1"
LE_ADV_IFACE = "org.bluez.LEAdvertisement1"
AGENT_IFACE = "org.bluez.Agent1"
AGENT_MANAGER_IFACE = "org.bluez.AgentManager1"
DBUS_OM_IFACE = "org.freedesktop.DBus.ObjectManager"
DBUS_PROP_IFACE = "org.freedesktop.DBus.Properties"

APP_PATH = "/org/porthole/hid"
ADV_PATH = "/org/porthole/adv0"
AGENT_PATH = "/org/porthole/agent"

REPO_ROOT = Path(__file__).resolve().parents[2]
HID_C = REPO_ROOT / "src" / "hid" / "hid_reports.c"
HID_H = REPO_ROOT / "src" / "hid" / "hid_reports.h"

MOD_LSHIFT = 0x02


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


# --------------------------------------------------------------------------- HID data

def load_report_descriptor():
    """Extracts ph_hid_report_descriptor[] from the C source."""
    header = HID_H.read_text()
    ids = {m.group(1): int(m.group(2))
           for m in re.finditer(r"(PH_HID_REPORT_ID_\w+)\s*=\s*(\d+)", header)}
    src = HID_C.read_text()
    body = re.search(r"ph_hid_report_descriptor\[\]\s*=\s*\{(.*?)\};", src, re.S).group(1)
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    values = []
    for tok in (t.strip() for t in body.split(",")):
        if tok:
            values.append(ids[tok] if tok in ids else int(tok, 0))
    return bytes(values), ids


def mouse_report(buttons=0, dx=0, dy=0, wheel=0, pan=0):
    return struct.pack("<Bhhbb", buttons, dx, dy, wheel, pan)


def keyboard_report(mods=0, keys=()):
    keys = list(keys)[:6]
    return bytes([mods, 0] + keys + [0] * (6 - len(keys)))


def consumer_report(usage=0):
    return struct.pack("<H", usage)


_PUNCT = [("-", "_", 0x2D), ("=", "+", 0x2E), ("[", "{", 0x2F), ("]", "}", 0x30),
          ("\\", "|", 0x31), (";", ":", 0x33), ("'", '"', 0x34), ("`", "~", 0x35),
          (",", "<", 0x36), (".", ">", 0x37), ("/", "?", 0x38)]


def ascii_to_key(c):
    """Same mapping as ph_hid_ascii_to_key() in the C code (US layout)."""
    if "a" <= c <= "z":
        return 0x04 + ord(c) - ord("a"), 0
    if "A" <= c <= "Z":
        return 0x04 + ord(c) - ord("A"), MOD_LSHIFT
    if "1" <= c <= "9":
        return 0x1E + ord(c) - ord("1"), 0
    simple = {"0": 0x27, "\n": 0x28, "\t": 0x2B, " ": 0x2C}
    if c in simple:
        return simple[c], 0
    if c in "!@#$%^&*()":
        return 0x1E + "!@#$%^&*()".index(c), MOD_LSHIFT
    for plain, shifted, usage in _PUNCT:
        if c == plain:
            return usage, 0
        if c == shifted:
            return usage, MOD_LSHIFT
    return None


# --------------------------------------------------------------------------- GATT objects

class Application(dbus.service.Object):
    def __init__(self, bus):
        self.path = APP_PATH
        self.services = []
        super().__init__(bus, self.path)

    def get_path(self):
        return dbus.ObjectPath(self.path)

    @dbus.service.method(DBUS_OM_IFACE, out_signature="a{oa{sa{sv}}}")
    def GetManagedObjects(self):
        objects = {}
        for service in self.services:
            objects[service.get_path()] = service.get_properties()
            for chrc in service.characteristics:
                objects[chrc.get_path()] = chrc.get_properties()
                for desc in chrc.descriptors:
                    objects[desc.get_path()] = desc.get_properties()
        return objects


class Service(dbus.service.Object):
    def __init__(self, bus, app, uuid):
        self.path = f"{app.path}/service{len(app.services)}"
        self.bus = bus
        self.uuid = uuid
        self.characteristics = []
        super().__init__(bus, self.path)
        app.services.append(self)

    def get_path(self):
        return dbus.ObjectPath(self.path)

    def get_properties(self):
        return {GATT_SERVICE_IFACE: {
            "UUID": self.uuid,
            "Primary": True,
            "Characteristics": dbus.Array([c.get_path() for c in self.characteristics],
                                          signature="o"),
        }}

    def add(self, uuid, flags, value=b"", name=None, on_write=None):
        chrc = Characteristic(self, uuid, flags, value, name or uuid, on_write)
        self.characteristics.append(chrc)
        return chrc


class Characteristic(dbus.service.Object):
    def __init__(self, service, uuid, flags, value, name, on_write):
        self.path = f"{service.path}/char{len(service.characteristics)}"
        self.service = service
        self.uuid = uuid
        self.flags = flags
        self.value = bytes(value)
        self.name = name
        self.on_write = on_write
        self.notifying = False
        self.subscribed_at = 0.0  # time of the last StartNotify
        self.descriptors = []
        super().__init__(service.bus, self.path)

    def get_path(self):
        return dbus.ObjectPath(self.path)

    def get_properties(self):
        return {GATT_CHRC_IFACE: {
            "Service": self.service.get_path(),
            "UUID": self.uuid,
            "Flags": self.flags,
            "Descriptors": dbus.Array([d.get_path() for d in self.descriptors], signature="o"),
        }}

    def add_descriptor(self, uuid, flags, value):
        desc = Descriptor(self, uuid, flags, value)
        self.descriptors.append(desc)
        return desc

    @dbus.service.method(DBUS_PROP_IFACE, in_signature="s", out_signature="a{sv}")
    def GetAll(self, interface):
        return self.get_properties()[GATT_CHRC_IFACE]

    @dbus.service.method(GATT_CHRC_IFACE, in_signature="a{sv}", out_signature="ay")
    def ReadValue(self, options):
        offset = int(options.get("offset", 0))
        return dbus.Array(self.value[offset:], signature="y")

    @dbus.service.method(GATT_CHRC_IFACE, in_signature="aya{sv}")
    def WriteValue(self, value, options):
        self.value = bytes(value)
        log(f"iPhone wrote {self.name}: {self.value.hex()}")
        if self.on_write:
            self.on_write(self.value)

    @dbus.service.method(GATT_CHRC_IFACE)
    def StartNotify(self):
        self.notifying = True
        self.subscribed_at = time.time()
        log(f"iPhone subscribed to {self.name}")

    @dbus.service.method(GATT_CHRC_IFACE)
    def StopNotify(self):
        self.notifying = False
        log(f"iPhone unsubscribed from {self.name}")

    @dbus.service.signal(DBUS_PROP_IFACE, signature="sa{sv}as")
    def PropertiesChanged(self, interface, changed, invalidated):
        pass

    def notify(self, value):
        """Must run on the GLib main loop thread.

        Always emits, even without a StartNotify: a bonded iPhone doesn't re-subscribe
        after reconnecting (it expects the subscription to be remembered), and BlueZ
        only forwards the value to clients whose subscription it has on record."""
        self.value = bytes(value)
        self.PropertiesChanged(GATT_CHRC_IFACE,
                               {"Value": dbus.Array(self.value, signature="y")}, [])
        return False  # for GLib.idle_add


class Descriptor(dbus.service.Object):
    def __init__(self, chrc, uuid, flags, value):
        self.path = f"{chrc.path}/desc{len(chrc.descriptors)}"
        self.chrc = chrc
        self.uuid = uuid
        self.flags = flags
        self.value = bytes(value)
        super().__init__(chrc.service.bus, self.path)

    def get_path(self):
        return dbus.ObjectPath(self.path)

    def get_properties(self):
        return {GATT_DESC_IFACE: {
            "Characteristic": self.chrc.get_path(),
            "UUID": self.uuid,
            "Flags": self.flags,
        }}

    @dbus.service.method(DBUS_PROP_IFACE, in_signature="s", out_signature="a{sv}")
    def GetAll(self, interface):
        return self.get_properties()[GATT_DESC_IFACE]

    @dbus.service.method(GATT_DESC_IFACE, in_signature="a{sv}", out_signature="ay")
    def ReadValue(self, options):
        offset = int(options.get("offset", 0))
        return dbus.Array(self.value[offset:], signature="y")


class Advertisement(dbus.service.Object):
    def __init__(self, bus, name):
        self.name = name
        super().__init__(bus, ADV_PATH)

    def get_path(self):
        return dbus.ObjectPath(ADV_PATH)

    @dbus.service.method(DBUS_PROP_IFACE, in_signature="s", out_signature="a{sv}")
    def GetAll(self, interface):
        return {
            "Type": "peripheral",
            "ServiceUUIDs": dbus.Array(["1812"], signature="s"),
            "Appearance": dbus.UInt16(0x03C0),  # Generic HID
            "LocalName": dbus.String(self.name),
            "Discoverable": dbus.Boolean(True),
        }

    @dbus.service.method(LE_ADV_IFACE)
    def Release(self):
        log("advertisement released")


class Agent(dbus.service.Object):
    """Accepts pairing without a code ("Just Works")."""

    def __init__(self, bus):
        super().__init__(bus, AGENT_PATH)

    @dbus.service.method(AGENT_IFACE)
    def Release(self):
        pass

    @dbus.service.method(AGENT_IFACE, in_signature="os")
    def AuthorizeService(self, device, uuid):
        log(f"authorize service {uuid} for {device}: yes")

    @dbus.service.method(AGENT_IFACE, in_signature="o", out_signature="s")
    def RequestPinCode(self, device):
        return "0000"

    @dbus.service.method(AGENT_IFACE, in_signature="o", out_signature="u")
    def RequestPasskey(self, device):
        return dbus.UInt32(0)

    @dbus.service.method(AGENT_IFACE, in_signature="ouq")
    def DisplayPasskey(self, device, passkey, entered):
        log(f"passkey for {device}: {passkey:06d}")

    @dbus.service.method(AGENT_IFACE, in_signature="os")
    def DisplayPinCode(self, device, pincode):
        log(f"PIN for {device}: {pincode}")

    @dbus.service.method(AGENT_IFACE, in_signature="ou")
    def RequestConfirmation(self, device, passkey):
        log(f"pairing with {device} (code {passkey:06d}): accepted")

    @dbus.service.method(AGENT_IFACE, in_signature="o")
    def RequestAuthorization(self, device):
        log(f"pairing request from {device}: accepted")

    @dbus.service.method(AGENT_IFACE)
    def Cancel(self):
        log("pairing cancelled")


# --------------------------------------------------------------------------- HID service

class HidDevice:
    def __init__(self, bus, app, descriptor, ids):
        enc_r = ["encrypt-read"]
        hid = Service(bus, app, "1812")
        hid.add("2a4a", enc_r, bytes([0x11, 0x01, 0x00, 0x02]), "HID Information")
        hid.add("2a4b", enc_r, descriptor, "Report Map")
        hid.add("2a4c", ["encrypt-write", "write-without-response"], b"\x00",
                "HID Control Point")
        hid.add("2a4e", enc_r + ["encrypt-write", "write-without-response"], b"\x01",
                "Protocol Mode")

        def input_report(report_id, name, length):
            chrc = hid.add("2a4d", enc_r + ["notify"], bytes(length), name)
            chrc.add_descriptor("2908", ["read"], bytes([report_id, 0x01]))
            return chrc

        self.keyboard = input_report(ids["PH_HID_REPORT_ID_KEYBOARD"], "keyboard report", 8)
        self.mouse = input_report(ids["PH_HID_REPORT_ID_MOUSE"], "mouse report", 7)
        self.consumer = input_report(ids["PH_HID_REPORT_ID_CONSUMER"], "consumer report", 2)
        leds = hid.add("2a4d", enc_r + ["encrypt-write", "write-without-response"], b"\x00",
                       "keyboard LED report")
        leds.add_descriptor("2908", ["read"], bytes([ids["PH_HID_REPORT_ID_KEYBOARD"], 0x02]))

        battery = Service(bus, app, "180f")
        battery.add("2a19", ["read", "notify"], bytes([100]), "Battery Level")

        info = Service(bus, app, "180a")
        info.add("2a29", ["read"], b"Porthole", "Manufacturer Name")
        # PnP ID: USB vendor source, pid.codes test VID 0x1209 / PID 0x0001, version 0x0001
        info.add("2a50", ["read"], struct.pack("<BHHH", 0x02, 0x1209, 0x0001, 0x0001), "PnP ID")


# --------------------------------------------------------------------------- commands

class CommandRunner:
    def __init__(self, hid, fifo_path, loop, readvertise, reregister):
        self.hid = hid
        self.fifo_path = fifo_path
        self.loop = loop
        self.readvertise = readvertise
        self.reregister = reregister
        self.buttons = 0
        self.interval = 0.015
        self.connected = {}

    def send(self, chrc, payload):
        GLib.idle_add(chrc.notify, payload)
        time.sleep(self.interval)

    def mouse(self, dx=0, dy=0, wheel=0, pan=0):
        self.send(self.hid.mouse, mouse_report(self.buttons, dx, dy, wheel, pan))

    def move(self, dx, dy, step=20):
        n = max(1, math.ceil(max(abs(dx), abs(dy)) / max(1, step)))
        for i in range(n):
            sx = round(dx * (i + 1) / n) - round(dx * i / n)
            sy = round(dy * (i + 1) / n) - round(dy * i / n)
            self.mouse(sx, sy)

    def key(self, usage, mods=0):
        self.send(self.hid.keyboard, keyboard_report(mods, [usage]))
        self.send(self.hid.keyboard, keyboard_report())

    def run(self, line):
        parts = line.split()
        if not parts:
            return
        cmd, args = parts[0].lower(), parts[1:]
        num = [int(a, 0) for a in args] if cmd not in ("type", "path") else []
        if cmd == "path":
            for tok in args:
                step, count = (int(v) for v in tok[1:].split("*"))
                for _ in range(count):
                    if tok[0] == "x":
                        self.mouse(step, 0)
                    else:
                        self.mouse(0, step)
        elif cmd == "move":
            self.move(num[0], num[1], num[2] if len(num) > 2 else 20)
        elif cmd == "anchor":
            for _ in range(60):
                self.mouse(-127, -127)
        elif cmd == "click":
            bit = 1 << ((num[0] if num else 1) - 1)
            self.buttons |= bit
            self.mouse()
            self.buttons &= ~bit
            self.mouse()
        elif cmd in ("down", "up"):
            bit = 1 << (num[0] - 1)
            self.buttons = self.buttons | bit if cmd == "down" else self.buttons & ~bit
            self.mouse()
        elif cmd == "drag":
            self.buttons |= 1
            self.mouse()
            self.move(num[0], num[1], num[2] if len(num) > 2 else 20)
            self.buttons &= ~1
            self.mouse()
        elif cmd == "scroll":
            self.mouse(wheel=num[0])
            self.mouse()
        elif cmd == "pan":
            self.mouse(pan=num[0])
            self.mouse()
        elif cmd == "key":
            self.key(num[0], num[1] if len(num) > 1 else 0)
        elif cmd == "type":
            text = line.split(None, 1)[1] if len(parts) > 1 else ""
            for c in text:
                mapped = ascii_to_key(c)
                if mapped:
                    self.key(*mapped)
                else:
                    log(f"can't type {c!r}")
        elif cmd == "consumer":
            self.send(self.hid.consumer, consumer_report(num[0]))
            self.send(self.hid.consumer, consumer_report(0))
        elif cmd == "interval":
            self.interval = num[0] / 1000.0
        elif cmd == "sleep":
            time.sleep(num[0] / 1000.0)
        elif cmd == "status":
            subs = [c.name for c in (self.hid.mouse, self.hid.keyboard, self.hid.consumer)
                    if c.notifying]
            log(f"connected: {self.connected or 'none'}; subscribed: {subs or 'none'}")
        elif cmd == "readvertise":
            GLib.idle_add(self.readvertise)
        elif cmd == "reregister":
            GLib.idle_add(self.reregister)
        elif cmd == "quit":
            GLib.idle_add(self.loop.quit)
        else:
            log(f"unknown command: {cmd}")
            return
        log(f"done: {line}")

    def serve(self):
        if not os.path.exists(self.fifo_path):
            os.mkfifo(self.fifo_path, 0o600)
        log(f"reading commands from {self.fifo_path}")
        while True:
            with open(self.fifo_path) as fifo:  # blocks until a writer opens it
                for line in fifo:
                    try:
                        self.run(line.strip())
                    except (IndexError, ValueError) as e:
                        log(f"bad command {line.strip()!r}: {e}")


# --------------------------------------------------------------------------- main

def find_adapter(bus):
    objects = dbus.Interface(bus.get_object(BLUEZ, "/"), DBUS_OM_IFACE).GetManagedObjects()
    for path, ifaces in objects.items():
        if GATT_MANAGER_IFACE in ifaces and LE_ADV_MANAGER_IFACE in ifaces:
            return path
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--name", default="Porthole", help="Bluetooth name (default: Porthole)")
    runtime = os.environ.get("XDG_RUNTIME_DIR", "/tmp")
    parser.add_argument("--fifo", default=f"{runtime}/porthole-hid.fifo")
    args = parser.parse_args()

    descriptor, ids = load_report_descriptor()
    log(f"HID report descriptor: {len(descriptor)} bytes from {HID_C.relative_to(REPO_ROOT)}")

    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()
    adapter_path = find_adapter(bus)
    if not adapter_path:
        sys.exit("No Bluetooth adapter with LE peripheral support found.")
    adapter = bus.get_object(BLUEZ, adapter_path)
    adapter_props = dbus.Interface(adapter, DBUS_PROP_IFACE)
    old_alias = adapter_props.Get(ADAPTER_IFACE, "Alias")
    adapter_props.Set(ADAPTER_IFACE, "Powered", dbus.Boolean(True))
    adapter_props.Set(ADAPTER_IFACE, "Pairable", dbus.Boolean(True))
    adapter_props.Set(ADAPTER_IFACE, "Alias", dbus.String(args.name))
    log(f"adapter {adapter_path} ({adapter_props.Get(ADAPTER_IFACE, 'Address')}) "
        f"renamed '{old_alias}' -> '{args.name}'")

    loop = GLib.MainLoop()
    app = Application(bus)
    hid = HidDevice(bus, app, descriptor, ids)
    adv = Advertisement(bus, args.name)
    agent = Agent(bus)
    adv_mgr = dbus.Interface(adapter, LE_ADV_MANAGER_IFACE)

    agent_mgr = dbus.Interface(bus.get_object(BLUEZ, "/org/bluez"), AGENT_MANAGER_IFACE)
    agent_mgr.RegisterAgent(AGENT_PATH, "NoInputNoOutput")
    agent_mgr.RequestDefaultAgent(AGENT_PATH)

    def fail(what):
        def handler(error):
            log(f"{what} failed: {error}")
            loop.quit()
        return handler

    def advertise(fatal=True):
        adv_mgr.RegisterAdvertisement(
            adv.get_path(), {},
            reply_handler=lambda: log(f"advertising as '{args.name}' - pair it from the "
                                      "iPhone's Settings > Bluetooth"),
            error_handler=fail("advertising") if fatal else
            (lambda e: log(f"re-advertising failed (will retry on next command): {e}")))
        return False

    def readvertise():
        try:
            adv_mgr.UnregisterAdvertisement(ADV_PATH)
        except dbus.exceptions.DBusException as e:
            log(f"unregister advertisement: {e.get_dbus_message()}")
        # BlueZ removes the old instance asynchronously; give it time before re-adding.
        GLib.timeout_add(500, advertise, False)
        return False

    gatt_mgr = dbus.Interface(adapter, GATT_MANAGER_IFACE)

    def register_app(fatal=True):
        gatt_mgr.RegisterApplication(
            app.get_path(), {},
            reply_handler=lambda: log("HID service registered"),
            error_handler=fail("registering HID service") if fatal else
            (lambda e: log(f"re-registering HID service failed: {e}")))
        return False

    def reregister():
        """Removes and re-adds the HID service. BlueZ then tells the iPhone the services
        changed, which makes it rediscover them and subscribe to the reports again."""
        try:
            gatt_mgr.UnregisterApplication(APP_PATH)
        except dbus.exceptions.DBusException as e:
            log(f"unregister HID service: {e.get_dbus_message()}")
        GLib.timeout_add(500, register_app, False)
        return False

    runner = CommandRunner(hid, args.fifo, loop, readvertise, reregister)
    register_app()
    advertise()

    def check_resubscribed(connected_at):
        # A bonded iPhone doesn't re-subscribe after reconnecting and BlueZ doesn't keep
        # our subscriptions, so reports would silently go nowhere. Force rediscovery.
        if hid.mouse.subscribed_at < connected_at:
            log("iPhone reconnected without subscribing; re-registering the HID service")
            reregister()
        return False

    def on_device_changed(interface, changed, invalidated, path=None):
        if interface != DEVICE_IFACE:
            return
        dev = dbus.Interface(bus.get_object(BLUEZ, path), DBUS_PROP_IFACE)
        name = str(dev.Get(DEVICE_IFACE, "Alias"))
        for key in ("Connected", "Paired", "Bonded"):
            if key in changed:
                log(f"{name}: {key} = {bool(changed[key])}")
        if "Connected" in changed:
            if changed["Connected"]:
                runner.connected[str(path)] = name
                GLib.timeout_add(3000, check_resubscribed, time.time())
            else:
                runner.connected.pop(str(path), None)
        if changed.get("Paired"):
            dev.Set(DEVICE_IFACE, "Trusted", dbus.Boolean(True))
            log(f"{name}: trusted (will reconnect automatically)")

    bus.add_signal_receiver(on_device_changed, dbus_interface=DBUS_PROP_IFACE,
                            signal_name="PropertiesChanged", path_keyword="path")

    threading.Thread(target=runner.serve, daemon=True).start()
    for sig in (signal.SIGINT, signal.SIGTERM):
        GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, sig, loop.quit)

    try:
        loop.run()
    finally:
        log("shutting down")
        for call in (
            lambda: adv_mgr.UnregisterAdvertisement(ADV_PATH),
            lambda: dbus.Interface(adapter, GATT_MANAGER_IFACE).UnregisterApplication(APP_PATH),
            lambda: agent_mgr.UnregisterAgent(AGENT_PATH),
            lambda: adapter_props.Set(ADAPTER_IFACE, "Alias", old_alias),
        ):
            try:
                call()
            except dbus.exceptions.DBusException:
                pass


if __name__ == "__main__":
    main()
