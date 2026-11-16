#!/usr/bin/env python3
"""Porthole Bluetooth helper for Linux (BlueZ).

Makes this computer a Bluetooth LE keyboard + mouse ("HID over GATT") for the iPhone.
The Porthole app starts it and talks to it over stdin/stdout:

stdin, binary frames  [type: 1 byte][length: 1 byte][payload]
  1  keyboard report (8 bytes)      2  mouse report (7 bytes)
  3  consumer report (2 bytes)      16 sync (4 bytes, little-endian id)
  17 report interval (2 bytes, little-endian milliseconds)
  18 pause after preceding reports (2 bytes, little-endian milliseconds)
Reports are sent one at a time, a fixed interval apart (default 15 ms): iOS pointer
acceleration depends on the timing, and Porthole's pointer model was measured at 15 ms.

stdout, text lines
  ready | advertising | connected | disconnected | subscribed | sync <id> | error <message>

The helper exits when stdin closes. The HID report descriptor is read from
src/hid/hid_reports.c so the app and the helper always agree on the report layout.
"""

import os
import queue
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

FRAME_KEYBOARD, FRAME_MOUSE, FRAME_CONSUMER = 1, 2, 3
FRAME_SYNC, FRAME_INTERVAL = 16, 17
FRAME_DELAY = 18

DEFAULT_DESCRIPTOR_SOURCE = Path(__file__).resolve().parents[2] / "src" / "hid" / "hid_reports.c"

_out_lock = threading.Lock()


def emit(line):
    """One status line to the app."""
    with _out_lock:
        sys.stdout.write(line + "\n")
        sys.stdout.flush()


def log(msg):
    sys.stderr.write(f"[porthole-ble] {msg}\n")
    sys.stderr.flush()


def load_report_descriptor(c_file):
    header = c_file.with_suffix(".h").read_text()
    ids = {m.group(1): int(m.group(2))
           for m in re.finditer(r"(PH_HID_REPORT_ID_\w+)\s*=\s*(\d+)", header)}
    body = re.search(r"ph_hid_report_descriptor\[\]\s*=\s*\{(.*?)\};", c_file.read_text(),
                     re.S).group(1)
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    values = [ids[t] if t in ids else int(t, 0) for t in (s.strip() for s in body.split(",")) if t]
    return bytes(values), ids


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

    def add(self, uuid, flags, value=b"", name=None):
        chrc = Characteristic(self, uuid, flags, value, name or uuid)
        self.characteristics.append(chrc)
        return chrc


class Characteristic(dbus.service.Object):
    def __init__(self, service, uuid, flags, value, name):
        self.path = f"{service.path}/char{len(service.characteristics)}"
        self.service = service
        self.uuid = uuid
        self.flags = flags
        self.value = bytes(value)
        self.name = name
        self.subscribed_at = 0.0
        self.descriptors = []
        self.on_subscribe = None
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
        return dbus.Array(self.value[int(options.get("offset", 0)):], signature="y")

    @dbus.service.method(GATT_CHRC_IFACE, in_signature="aya{sv}")
    def WriteValue(self, value, options):
        self.value = bytes(value)

    @dbus.service.method(GATT_CHRC_IFACE)
    def StartNotify(self):
        self.subscribed_at = time.time()
        log(f"subscribed: {self.name}")
        if self.on_subscribe:
            self.on_subscribe()

    @dbus.service.method(GATT_CHRC_IFACE)
    def StopNotify(self):
        pass

    @dbus.service.signal(DBUS_PROP_IFACE, signature="sa{sv}as")
    def PropertiesChanged(self, interface, changed, invalidated):
        pass

    def notify(self, value):
        """Main-loop thread only. Always emits: a bonded iPhone doesn't re-subscribe after a
        reconnect, and BlueZ only forwards to clients whose subscription it has on record."""
        self.value = bytes(value)
        self.PropertiesChanged(GATT_CHRC_IFACE, {"Value": dbus.Array(self.value, signature="y")},
                               [])
        return False


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
        return dbus.Array(self.value[int(options.get("offset", 0)):], signature="y")


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
            "Appearance": dbus.UInt16(0x03C0),  # generic HID
            "LocalName": dbus.String(self.name),
            "Discoverable": dbus.Boolean(True),
        }

    @dbus.service.method(LE_ADV_IFACE)
    def Release(self):
        pass


class Agent(dbus.service.Object):
    """Accepts pairing requests without a code ("Just Works")."""

    @dbus.service.method(AGENT_IFACE)
    def Release(self):
        pass

    @dbus.service.method(AGENT_IFACE, in_signature="os")
    def AuthorizeService(self, device, uuid):
        pass

    @dbus.service.method(AGENT_IFACE, in_signature="o", out_signature="s")
    def RequestPinCode(self, device):
        return "0000"

    @dbus.service.method(AGENT_IFACE, in_signature="o", out_signature="u")
    def RequestPasskey(self, device):
        return dbus.UInt32(0)

    @dbus.service.method(AGENT_IFACE, in_signature="ouq")
    def DisplayPasskey(self, device, passkey, entered):
        pass

    @dbus.service.method(AGENT_IFACE, in_signature="os")
    def DisplayPinCode(self, device, pincode):
        pass

    @dbus.service.method(AGENT_IFACE, in_signature="ou")
    def RequestConfirmation(self, device, passkey):
        log(f"pairing with {device}: accepted")

    @dbus.service.method(AGENT_IFACE, in_signature="o")
    def RequestAuthorization(self, device):
        log(f"pairing request from {device}: accepted")

    @dbus.service.method(AGENT_IFACE)
    def Cancel(self):
        pass


def build_hid(bus, app, descriptor, ids):
    enc_r = ["encrypt-read"]
    hid = Service(bus, app, "1812")
    hid.add("2a4a", enc_r, bytes([0x11, 0x01, 0x00, 0x02]), "HID Information")
    hid.add("2a4b", enc_r, descriptor, "Report Map")
    hid.add("2a4c", ["encrypt-write", "write-without-response"], b"\x00", "HID Control Point")
    hid.add("2a4e", enc_r + ["encrypt-write", "write-without-response"], b"\x01", "Protocol Mode")

    def input_report(report_id, name, length):
        chrc = hid.add("2a4d", enc_r + ["notify"], bytes(length), name)
        chrc.add_descriptor("2908", ["read"], bytes([report_id, 0x01]))
        return chrc

    reports = {
        FRAME_KEYBOARD: input_report(ids["PH_HID_REPORT_ID_KEYBOARD"], "keyboard", 8),
        FRAME_MOUSE: input_report(ids["PH_HID_REPORT_ID_MOUSE"], "mouse", 7),
        FRAME_CONSUMER: input_report(ids["PH_HID_REPORT_ID_CONSUMER"], "consumer", 2),
    }
    leds = hid.add("2a4d", enc_r + ["encrypt-write", "write-without-response"], b"\x00",
                   "keyboard LEDs")
    leds.add_descriptor("2908", ["read"], bytes([ids["PH_HID_REPORT_ID_KEYBOARD"], 0x02]))

    battery = Service(bus, app, "180f")
    battery.add("2a19", ["read", "notify"], bytes([100]), "Battery Level")

    info = Service(bus, app, "180a")
    info.add("2a29", ["read"], b"Porthole", "Manufacturer Name")
    info.add("2a50", ["read"], struct.pack("<BHHH", 0x02, 0x1209, 0x0001, 0x0001), "PnP ID")
    return reports


# --------------------------------------------------------------------------- stdin protocol

EXPECTED_LEN = {FRAME_KEYBOARD: 8, FRAME_MOUSE: 7, FRAME_CONSUMER: 2, FRAME_SYNC: 4,
                FRAME_INTERVAL: 2, FRAME_DELAY: 2}


def read_exact(stream, n):
    data = b""
    while len(data) < n:
        chunk = stream.read(n - len(data))
        if not chunk:
            return None
        data += chunk
    return data


def stdin_reader(out_queue):
    """Parses frames from the app; puts (type, payload) on the queue; None at EOF."""
    # This daemon thread can still be blocked when advertising fails and main exits.
    # An unbuffered duplicate avoids holding stdin's BufferedReader lock at shutdown.
    stream = os.fdopen(os.dup(sys.stdin.fileno()), "rb", buffering=0)
    while True:
        header = read_exact(stream, 2)
        if header is None:
            break
        ftype, length = header[0], header[1]
        payload = read_exact(stream, length)
        if payload is None:
            break
        if EXPECTED_LEN.get(ftype) != length:
            emit(f"error bad frame type {ftype} length {length}")
            continue
        out_queue.put((ftype, payload))
    out_queue.put(None)


def sender(in_queue, reports, loop):
    """Sends queued reports one per interval on the GLib main loop."""
    interval = 0.015
    while True:
        item = in_queue.get()
        if item is None:
            GLib.idle_add(loop.quit)
            return
        ftype, payload = item
        if ftype == FRAME_SYNC:
            emit(f"sync {struct.unpack('<I', payload)[0]}")
        elif ftype == FRAME_INTERVAL:
            interval = max(0.005, struct.unpack("<H", payload)[0] / 1000.0)
        elif ftype == FRAME_DELAY:
            time.sleep(struct.unpack("<H", payload)[0] / 1000.0)
        else:
            sent = threading.Event()

            def dispatch(chrc=reports[ftype], value=payload, done=sent):
                try:
                    chrc.notify(value)
                finally:
                    done.set()
                return False

            GLib.idle_add(dispatch)
            # A busy main loop must not accumulate notifications and then flush them
            # in a burst. Measure the interval from the actual dispatch, and let sync
            # acknowledge only reports whose callbacks have finished.
            sent.wait()
            time.sleep(interval)


# --------------------------------------------------------------------------- main

def find_adapter(bus):
    if not bus.name_has_owner(BLUEZ):
        return None  # Don't wait 25 s for D-Bus to activate BlueZ without an adapter.
    objects = dbus.Interface(bus.get_object(BLUEZ, "/"), DBUS_OM_IFACE).GetManagedObjects()
    for path, ifaces in objects.items():
        if GATT_MANAGER_IFACE in ifaces and LE_ADV_MANAGER_IFACE in ifaces:
            return path
    return None


def reports_subscribed(reports, connected_at=0.0):
    """The composite device is usable only after all three input reports subscribe."""
    return all(report.subscribed_at > 0 and report.subscribed_at >= connected_at
               for report in reports.values())


def main():
    name = os.environ.get("PORTHOLE_BLE_NAME", "Porthole")
    source = Path(os.environ.get("PORTHOLE_HID_SOURCE", str(DEFAULT_DESCRIPTOR_SOURCE)))
    try:
        descriptor, ids = load_report_descriptor(source)
    except (OSError, AttributeError, KeyError, ValueError) as e:
        emit(f"error can't read the HID descriptor from {source}: {e}")
        return 1

    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    try:
        bus = dbus.SystemBus()
        adapter_path = find_adapter(bus)
    except dbus.exceptions.DBusException as e:
        emit(f"error can't reach BlueZ: {e.get_dbus_message()}")
        return 1
    if not adapter_path:
        emit("error no Bluetooth adapter with LE peripheral support")
        return 1
    adapter = bus.get_object(BLUEZ, adapter_path)
    props = dbus.Interface(adapter, DBUS_PROP_IFACE)
    old_alias = props.Get(ADAPTER_IFACE, "Alias")
    props.Set(ADAPTER_IFACE, "Powered", dbus.Boolean(True))
    props.Set(ADAPTER_IFACE, "Pairable", dbus.Boolean(True))
    props.Set(ADAPTER_IFACE, "Alias", dbus.String(name))

    loop = GLib.MainLoop()

    def bluez_owner_changed(service, old_owner, new_owner):
        if old_owner:
            emit("error Bluetooth service stopped or restarted; reconnecting")
            loop.quit()

    def interfaces_removed(path, interfaces):
        if path == adapter_path and ADAPTER_IFACE in interfaces:
            emit("error Bluetooth adapter disconnected; reconnecting")
            loop.quit()

    bus.add_signal_receiver(bluez_owner_changed, dbus_interface="org.freedesktop.DBus",
                            signal_name="NameOwnerChanged", arg0=BLUEZ)
    bus.add_signal_receiver(interfaces_removed, dbus_interface=DBUS_OM_IFACE,
                            signal_name="InterfacesRemoved", bus_name=BLUEZ)
    app = Application(bus)
    reports = build_hid(bus, app, descriptor, ids)
    adv = Advertisement(bus, name)
    agent = Agent(bus, AGENT_PATH)
    gatt_mgr = dbus.Interface(adapter, GATT_MANAGER_IFACE)
    adv_mgr = dbus.Interface(adapter, LE_ADV_MANAGER_IFACE)
    agent_mgr = dbus.Interface(bus.get_object(BLUEZ, "/org/bluez"), AGENT_MANAGER_IFACE)
    agent_mgr.RegisterAgent(AGENT_PATH, "NoInputNoOutput")
    agent_mgr.RequestDefaultAgent(AGENT_PATH)

    def register_app(fatal=True):
        def failed(e):
            emit(f"error registering the HID service: {e}")
            if fatal:
                loop.quit()
        gatt_mgr.RegisterApplication(app.get_path(), {}, reply_handler=lambda: None,
                                     error_handler=failed)
        return False

    def reregister():
        """iOS rediscovers the services and subscribes again after this."""
        try:
            gatt_mgr.UnregisterApplication(APP_PATH)
        except dbus.exceptions.DBusException:
            pass
        GLib.timeout_add(500, register_app, False)
        return False

    def advertised():
        emit("advertising")

    def adv_failed(e):
        emit(f"error advertising: {e}")
        loop.quit()

    register_app()
    adv_mgr.RegisterAdvertisement(adv.get_path(), {}, reply_handler=advertised,
                                  error_handler=adv_failed)

    def drop_stale_links():
        """An iPhone still connected from an earlier session keeps its old subscriptions,
        which don't carry over to this service. Disconnecting makes it reconnect at once
        and subscribe afresh (tested: back within ~2 s)."""
        objects = dbus.Interface(bus.get_object(BLUEZ, "/"), DBUS_OM_IFACE).GetManagedObjects()
        for path, ifaces in objects.items():
            dev = ifaces.get(DEVICE_IFACE)
            if dev and dev.get("Connected") and not reports_subscribed(reports):
                log(f"dropping stale connection {path} so it subscribes again")
                try:
                    dbus.Interface(bus.get_object(BLUEZ, path), DEVICE_IFACE).Disconnect(
                        reply_handler=lambda: None,
                        error_handler=lambda e: log(f"disconnect failed: {e.get_dbus_message()}"))
                except dbus.exceptions.DBusException as e:
                    log(f"disconnect failed: {e.get_dbus_message()}")
        return False

    GLib.timeout_add(2000, drop_stale_links)

    subscribed_announced = [0.0]
    connected_since = [0.0]

    def on_subscribe():
        if (reports_subscribed(reports, connected_since[0])
                and time.time() - subscribed_announced[0] > 1.0):
            subscribed_announced[0] = time.time()
            emit("subscribed")
    for report in reports.values():
        report.on_subscribe = on_subscribe

    def reconnect_if_stale(path, connected_at):
        if reports_subscribed(reports, connected_at):
            return False
        try:
            device = bus.get_object(BLUEZ, path)
            if dbus.Interface(device, DBUS_PROP_IFACE).Get(DEVICE_IFACE, "Connected"):
                log("disconnecting the stale link so iOS subscribes to the new HID service")
                dbus.Interface(device, DEVICE_IFACE).Disconnect(
                    reply_handler=lambda: None,
                    error_handler=lambda e: log(f"disconnect failed: {e}"))
        except dbus.exceptions.DBusException as e:
            log(f"checking stale link failed: {e}")
        return False

    def check_resubscribed(connected_at, path):
        if not reports_subscribed(reports, connected_at):
            log("iPhone reconnected without subscribing; re-registering the HID service")
            reregister()
            GLib.timeout_add(1500, reconnect_if_stale, path, connected_at)
        return False

    def on_device_changed(interface, changed, invalidated, path=None):
        if interface != DEVICE_IFACE:
            return
        dev = dbus.Interface(bus.get_object(BLUEZ, path), DBUS_PROP_IFACE)
        if "Connected" in changed:
            if changed["Connected"]:
                connected_since[0] = time.time()
                emit("connected")
                GLib.timeout_add(3000, check_resubscribed, connected_since[0], str(path))
            else:
                emit("disconnected")
        if changed.get("Paired"):
            dev.Set(DEVICE_IFACE, "Trusted", dbus.Boolean(True))

    bus.add_signal_receiver(on_device_changed, dbus_interface=DBUS_PROP_IFACE,
                            signal_name="PropertiesChanged", path_keyword="path")

    frames = queue.Queue()
    threading.Thread(target=stdin_reader, args=(frames,), daemon=True).start()
    threading.Thread(target=sender, args=(frames, reports, loop), daemon=True).start()
    for sig in (signal.SIGINT, signal.SIGTERM):
        GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, sig, loop.quit)

    emit("ready")
    try:
        loop.run()
    finally:
        for call in (lambda: adv_mgr.UnregisterAdvertisement(ADV_PATH),
                     lambda: gatt_mgr.UnregisterApplication(APP_PATH),
                     lambda: agent_mgr.UnregisterAgent(AGENT_PATH),
                     lambda: props.Set(ADAPTER_IFACE, "Alias", old_alias)):
            try:
                call()
            except dbus.exceptions.DBusException:
                pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
