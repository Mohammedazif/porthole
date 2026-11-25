#!/usr/bin/env python3
"""Porthole Bluetooth helper for Linux (BlueZ).

Makes this computer a Bluetooth LE keyboard + mouse ("HID over GATT") for the iPhone.
The Porthole app starts it and talks to it over stdin/stdout:

stdin, binary frames  [type: 1 byte][length: 1 byte][payload]
  1  keyboard report (8 bytes)      2  mouse report (7 bytes)
  3  consumer report (2 bytes)      16 sync (4 bytes, little-endian id)
  17 report interval (2 bytes, little-endian milliseconds)
  18 pause after preceding reports (2 bytes, little-endian milliseconds)
Reports travel in two independent lanes, each sent one at a time, a fixed interval apart
(15 ms): iOS pointer acceleration depends on the timing, and Porthole's pointer model was
measured at 15 ms.
  motion lane: mouse reports, sync, interval and pause frames, strictly in order
  key lane:    keyboard and consumer reports, strictly in order
A key press therefore never waits behind queued pointer movement or a pause. "sync"
acknowledges the motion lane only.

stdout, text lines
  ready | advertising | connected | disconnected | subscribed | sync <id> | error <message>

The helper exits when stdin closes. The HID report descriptor is read from
src/hid/hid_reports.c so the app and the helper always agree on the report layout.
"""

import hashlib
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
DBUS_OM_IFACE = "org.freedesktop.DBus.ObjectManager"
DBUS_PROP_IFACE = "org.freedesktop.DBus.Properties"

APP_PATH = "/org/porthole/hid"
ADV_PATH = "/org/porthole/adv0"

FRAME_KEYBOARD, FRAME_MOUSE, FRAME_CONSUMER = 1, 2, 3
FRAME_ABS_MOUSE = 4  # experimental: [buttons][x lo][x hi][y lo][y hi][wheel], x/y 0..32767
FRAME_SYNC, FRAME_INTERVAL = 16, 17
FRAME_DELAY = 18
FRAME_PHONE_AUDIO = 19  # 1 byte: 1 = the phone plays its sound through this PC, 0 = on the phone
A2DP_SOURCE_UUID = "0000110a-0000-1000-8000-00805f9b34fb"  # the phone's audio, as a remote profile
PHONE_AUDIO_RETRY_S = 8  # between tries to bring the phone's sound here
PHONE_AUDIO_TRIES = 8  # per connection, about a minute
MEDIA_TRANSPORT_IFACE = "org.bluez.MediaTransport1"
KEY_LANE = (FRAME_KEYBOARD, FRAME_CONSUMER)

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


def absolute_mouse_collection(report_id):
    """Absolute pointer collection reported to work on iPhone under AssistiveTouch
    (esp32-iphone-agent abs_mouse.h): X/Y map straight to screen fractions."""
    return bytes([
        0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, report_id,
        0x09, 0x01, 0xA1, 0x00,
        0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01,
        0x75, 0x01, 0x95, 0x03, 0x81, 0x02,             # 3 buttons
        0x75, 0x05, 0x95, 0x01, 0x81, 0x01,             # padding
        0x05, 0x01, 0x09, 0x30, 0x09, 0x31,
        0x15, 0x00, 0x26, 0xFF, 0x7F, 0x35, 0x00, 0x46, 0xFF, 0x7F,
        0x75, 0x10, 0x95, 0x02, 0x81, 0x02,             # X, Y absolute 0..32767
        0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01,
        0x81, 0x06,                                     # wheel, relative
        0xC0, 0xC0,
    ])


def apply_absolute_experiment(descriptor, ids, mode):
    """PORTHOLE_HID_ABSOLUTE=add appends report 4; =replace swaps the relative mouse
    (report 2) for the absolute one. Returns (descriptor, absolute report id)."""
    if mode == "add":
        return descriptor + absolute_mouse_collection(4), 4
    if mode == "replace":
        mouse_id = ids["PH_HID_REPORT_ID_MOUSE"]
        start = descriptor.find(bytes([0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, mouse_id]))
        end = descriptor.find(bytes([0xC0, 0xC0]), start)
        if start < 0 or end < 0:
            raise ValueError("relative mouse collection not found")
        return descriptor[:start] + absolute_mouse_collection(mouse_id) + descriptor[end + 2:], mouse_id
    raise ValueError(f"unknown PORTHOLE_HID_ABSOLUTE mode {mode!r}")


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
        self.on_device = None
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
        self.app = app
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
        self.observe_device(options)
        return dbus.Array(self.value[int(options.get("offset", 0)):], signature="y")

    @dbus.service.method(GATT_CHRC_IFACE, in_signature="aya{sv}")
    def WriteValue(self, value, options):
        self.observe_device(options)
        self.value = bytes(value)

    def observe_device(self, options):
        callback = self.service.app.on_device
        if callback and options.get("device"):
            callback(str(options["device"]))

    @dbus.service.method(GATT_CHRC_IFACE)
    def StartNotify(self):
        self.subscribed_at = time.time()
        log(f"subscribed: {self.name}")
        if self.on_subscribe:
            self.on_subscribe()

    @dbus.service.method(GATT_CHRC_IFACE)
    def StopNotify(self):
        self.subscribed_at = 0.0

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


def build_hid(bus, app, descriptor, ids, absolute=None):
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

    reports = {FRAME_KEYBOARD: input_report(ids["PH_HID_REPORT_ID_KEYBOARD"], "keyboard", 8)}
    if absolute is None or absolute[0] == "add":
        reports[FRAME_MOUSE] = input_report(ids["PH_HID_REPORT_ID_MOUSE"], "mouse", 7)
    if absolute is not None:
        reports[FRAME_ABS_MOUSE] = input_report(absolute[1], "absolute mouse", 6)
    reports[FRAME_CONSUMER] = input_report(ids["PH_HID_REPORT_ID_CONSUMER"], "consumer", 2)
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

EXPECTED_LEN = {FRAME_KEYBOARD: 8, FRAME_MOUSE: 7, FRAME_ABS_MOUSE: 6, FRAME_CONSUMER: 2, FRAME_SYNC: 4,
                FRAME_INTERVAL: 2, FRAME_DELAY: 2, FRAME_PHONE_AUDIO: 1}


def read_exact(stream, n):
    data = b""
    while len(data) < n:
        chunk = stream.read(n - len(data))
        if not chunk:
            return None
        data += chunk
    return data


def stdin_reader(out_queue, key_queue=None):
    """Parses frames from the app; puts (type, payload) on a queue; None at EOF.

    With a key queue, keyboard and consumer reports go there so typing never waits
    behind queued pointer movement."""
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
        if key_queue is not None and ftype in KEY_LANE:
            key_queue.put((ftype, payload))
        else:
            out_queue.put((ftype, payload))
    if key_queue is not None:
        key_queue.put(None)
    out_queue.put(None)


def sender(in_queue, reports, loop, quit_at_eof=True, wait_for=None, stopping=None,
           on_phone_audio=None):
    """Sends one lane's queued reports; waits for the other lane before quitting."""
    interval = 0.015
    while True:
        if stopping is not None and stopping.is_set():
            return
        item = in_queue.get()
        if stopping is not None and stopping.is_set():
            return
        if item is None:
            if quit_at_eof:
                if wait_for is not None:
                    wait_for.join()
                GLib.idle_add(loop.quit)
            return
        ftype, payload = item
        if ftype == FRAME_SYNC:
            emit(f"sync {struct.unpack('<I', payload)[0]}")
        elif ftype == FRAME_INTERVAL:
            interval = max(0.005, struct.unpack("<H", payload)[0] / 1000.0)
        elif ftype == FRAME_DELAY:
            time.sleep(struct.unpack("<H", payload)[0] / 1000.0)
        elif ftype == FRAME_PHONE_AUDIO:
            if on_phone_audio is not None:
                GLib.idle_add(on_phone_audio, payload[0] == 1)
        elif ftype not in reports:
            continue  # e.g. a relative mouse report while the absolute map is active
        else:
            sent = threading.Event()

            def dispatch(chrc=reports[ftype], value=payload, done=sent):
                try:
                    if stopping is None or not stopping.is_set():
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


def release_reports(reports):
    """Main-loop thread, before unregistering: leave no held key/button behind."""
    for kind in (FRAME_KEYBOARD, FRAME_MOUSE, FRAME_CONSUMER):
        report = reports.get(kind)
        if report is None:
            continue
        try:
            report.notify(bytes(EXPECTED_LEN[kind]))
        except Exception as error:
            # A disappearing radio/service must not prevent the other releases
            # or provider/agent cleanup from being attempted.
            log(f"final neutral report {kind} failed: {error}")


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
        absolute = None
        mode = os.environ.get("PORTHOLE_HID_ABSOLUTE", "")
        if mode:
            descriptor, abs_id = apply_absolute_experiment(descriptor, ids, mode)
            absolute = (mode, abs_id)
            log(f"experimental absolute mouse ({mode}, report {abs_id}); "
                f"forget and re-pair the iPhone after changing this")
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
    try:
        if not props.Get(ADAPTER_IFACE, "Powered"):
            emit("error Turn on Bluetooth in this computer’s settings, then retry.")
            return 1
    except dbus.exceptions.DBusException as e:
        emit(f"error reading the Bluetooth adapter state: {e}")
        return 1

    loop = GLib.MainLoop()
    stopping = threading.Event()

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
    reports = build_hid(bus, app, descriptor, ids, absolute)
    adv = Advertisement(bus, name)
    gatt_mgr = dbus.Interface(adapter, GATT_MANAGER_IFACE)
    adv_mgr = dbus.Interface(adapter, LE_ADV_MANAGER_IFACE)

    advertising_started = [False]
    service_registered = [False]
    registration_generation = [0]
    refreshed_link = [False]
    reconnect_attempted = set()  # One recovery per peer for this helper's lifetime.

    def register_app(fatal=True):
        registration_generation[0] += 1
        registration = registration_generation[0]
        service_registered[0] = False

        def registered():
            if registration != registration_generation[0] or service_registered[0]:
                return
            service_registered[0] = True
            # BlueZ fetches our objects asynchronously. Advertising before that
            # finishes lets a fast reconnect discover an incomplete HID service.
            if not advertising_started[0]:
                advertising_started[0] = True
                adv_mgr.RegisterAdvertisement(adv.get_path(), {}, reply_handler=advertised,
                                              error_handler=adv_failed)
            # Restored CCCDs may arrive before the async registration reply.
            # Give the published provider its own full discovery grace.
            on_subscribe()
            arm_subscription_check()

        def failed(e):
            if registration != registration_generation[0]:
                return
            emit(f"error registering the HID service: {e}")
            if fatal:
                loop.quit()
        gatt_mgr.RegisterApplication(app.get_path(), {}, reply_handler=registered,
                                     error_handler=failed)
        return False

    def reregister():
        """iOS rediscovers the services and subscribes again after this."""
        service_registered[0] = False
        registration_generation[0] += 1  # retire timers/replies for the old provider
        for report in reports.values():
            report.subscribed_at = 0.0  # a replaced service cannot reuse its old CCC state
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

    subscribed_announced = [0.0]
    connected_since = [0.0]
    connected_path = [None]
    connection_generation = [0]
    subscription_peer = [None]
    devices = {}
    hid_devices = set()
    connection_observed = {}

    def selected_adapter(path):
        return str(path).startswith(str(adapter_path).rstrip("/") + "/")

    def phone_device(properties):
        name = str(properties.get("Alias", properties.get("Name", ""))).lower()
        return (properties.get("Icon") == "phone"
                or (int(properties.get("Class", 0)) & 0x1f00) == 0x0200
                or "iphone" in name)

    def bonded_and_trusted(path):
        """Never Disconnect an untrusted LE link: BlueZ blocks its incoming reconnects."""
        try:
            dev = dbus.Interface(bus.get_object(BLUEZ, path), DBUS_PROP_IFACE)
            try:
                bonded = bool(dev.Get(DEVICE_IFACE, "Bonded"))
            except dbus.exceptions.DBusException:
                bonded = bool(dev.Get(DEVICE_IFACE, "Paired"))
            return (bonded and bool(dev.Get(DEVICE_IFACE, "Trusted"))
                    and bool(dev.Get(DEVICE_IFACE, "Connected")))
        except dbus.exceptions.DBusException:
            return False

    def device_connected(path):
        try:
            dev = dbus.Interface(bus.get_object(BLUEZ, path), DBUS_PROP_IFACE)
            return bool(dev.Get(DEVICE_IFACE, "Connected"))
        except dbus.exceptions.DBusException:
            return False

    def emit_subscribed(path):
        """Names the phone by a hash of its Bluetooth device, so Porthole can tell
        the same phone returning from a different one without logging addresses."""
        emit("peer-id " + hashlib.sha256(str(path).encode()).hexdigest()[:16])
        emit("subscribed")

    def on_subscribe():
        if (service_registered[0] and connected_path[0]
                and device_connected(connected_path[0])
                and reports_subscribed(reports, connected_since[0])
                and time.time() - subscribed_announced[0] > 1.0):
            subscribed_announced[0] = time.time()
            subscription_peer[0] = connected_path[0]
            emit_subscribed(connected_path[0])
    for report in reports.values():
        report.on_subscribe = on_subscribe

    def subscription_deadline(connected_at, path, generation, registration):
        if (generation != connection_generation[0] or path != connected_path[0]
                or registration != registration_generation[0] or not service_registered[0]):
            return False
        if reports_subscribed(reports, connected_at):
            return False
        # Replacing a provider removes its native CCCDs. A bonded phone may
        # rediscover them only after reconnecting, but first let publication
        # finish and give it a full discovery grace. Never interrupt partial
        # subscriptions, fresh pairing, or repeatedly cycle a quiet peer.
        if (refreshed_link[0] and path not in reconnect_attempted
                and not any(report.subscribed_at > 0 for report in reports.values())
                and bonded_and_trusted(path)):
            # Property reads may dispatch D-Bus callbacks: recheck the captured
            # provider/connection and subscription progress immediately before
            # acting on that peer. Consume the attempt even if Disconnect fails.
            if (generation != connection_generation[0] or path != connected_path[0]
                    or registration != registration_generation[0] or not service_registered[0]
                    or any(report.subscribed_at > 0 for report in reports.values())):
                return False
            reconnect_attempted.add(path)
            log("HID service discovery timed out; reconnecting the bonded phone once (bond kept)")

            def disconnect_failed(error):
                log(f"reconnecting the bonded phone failed; waiting for the phone: {error}")

            try:
                dbus.Interface(bus.get_object(BLUEZ, path), DEVICE_IFACE).Disconnect(
                    reply_handler=lambda: None, error_handler=disconnect_failed)
            except dbus.exceptions.DBusException as error:
                disconnect_failed(error)
            return False
        log("HID service is registered; waiting for the phone to enable input reports (bond kept)")
        return False

    def check_resubscribed(connected_at, path, generation, registration):
        if (generation != connection_generation[0] or path != connected_path[0]
                or registration != registration_generation[0] or not service_registered[0]):
            return False
        if not bonded_and_trusted(path):
            return False  # Fresh pairing may still need user approval/encryption.
        if not reports_subscribed(reports, connected_at):
            if refreshed_link[0] or path in reconnect_attempted:
                return subscription_deadline(connected_at, path, generation, registration)
            refreshed_link[0] = True
            log("iPhone reconnected without subscribing; re-registering the HID service")
            reregister()
        return False

    def arm_subscription_check():
        if not service_registered[0] or not connected_path[0]:
            return
        if reports_subscribed(reports, connected_since[0]):
            return
        callback = subscription_deadline if refreshed_link[0] else check_resubscribed
        GLib.timeout_add(15000, callback, connected_since[0], connected_path[0],
                         connection_generation[0], registration_generation[0])

    def on_device_changed(interface, changed, invalidated, path=None):
        if interface != DEVICE_IFACE or path is None or not selected_adapter(path):
            return
        path = str(path)
        properties = devices.setdefault(path, {})
        paired_before = bool(properties.get("Paired"))
        was_connected = bool(properties.get("Connected"))
        properties.update(changed)
        if changed.get("Connected") and not was_connected:
            connection_observed[path] = time.time()
        elif "Connected" in changed and not changed["Connected"]:
            connection_observed.pop(path, None)
        eligible = path in hid_devices or phone_device(properties)
        if properties.get("Connected") and eligible and connected_path[0] is None:
            connection_generation[0] += 1
            refreshed_link[0] = False
            connected_since[0] = connection_observed.get(path, time.time())
            connected_path[0] = path
            if subscription_peer[0] not in (None, path):
                for report in reports.values():
                    report.subscribed_at = 0.0
                subscription_peer[0] = None
            emit("connected")
            reset_phone_audio_retry()
            if phone_audio[0]:
                GLib.timeout_add(1500, apply_phone_audio)  # once the phone link settles
            # Only the same bonded peer can reuse this service's enabled CCCDs.
            if (service_registered[0] and subscription_peer[0] == path and reports_subscribed(reports)
                    and bonded_and_trusted(path)):
                subscribed_announced[0] = time.time()
                emit_subscribed(path)
            elif (service_registered[0] and device_connected(path)
                  and reports_subscribed(reports, connected_since[0])):
                subscription_peer[0] = path
                subscribed_announced[0] = time.time()
                emit_subscribed(path)
            else:
                arm_subscription_check()
        if "Connected" in changed and not changed["Connected"] and path == connected_path[0]:
            connection_generation[0] += 1
            connected_path[0] = None
            emit("disconnected")
        if path == connected_path[0] and any(key in changed for key in ("Alias", "Name", "Connected")):
            device_name = str(properties.get("Alias") or properties.get("Name") or "")
            mac = str(properties.get("Address", "")).lower().replace(":", "-")
            device_name = " ".join(device_name.split())[:63]
            if device_name and device_name.lower().replace(":", "-") != mac:
                emit(f"peer-name {device_name}")
        if changed.get("Paired") and not paired_before and path == connected_path[0]:
            # The system pairing agent owns approval and trust. Give newly
            # paired devices a full discovery grace without changing their trust.
            connection_generation[0] += 1
            arm_subscription_check()

    def on_gatt_device(path):
        if not selected_adapter(path):
            return
        try:
            dev = dbus.Interface(bus.get_object(BLUEZ, path), DBUS_PROP_IFACE)
            if not dev.Get(DEVICE_IFACE, "Connected"):
                return  # A queued request from a closed link cannot resurrect it.
        except dbus.exceptions.DBusException:
            return
        hid_devices.add(str(path))
        # A server read/write proves the caller is connected even if its initial
        # Device1 object arrived already connected, without a property delta.
        on_device_changed(DEVICE_IFACE, {"Connected": True}, [], path=path)

    # ---- the phone's sound through this PC (BlueZ with PipeWire plays it) ----
    phone_audio = [None]  # None until Porthole says; then True (play here) or False
    transports = set()  # the phone's audio streams to this PC (MediaTransport1 paths)

    def audio_phone_path():
        """A paired phone that can send audio here, the connected one first.

        One pairing over LE also gives the iPhone and this PC a Classic key
        (cross-transport key derivation), so the connected phone counts before
        its Classic services are known: BlueZ reads them while connecting."""
        candidates = []
        for path, properties in devices.items():
            uuids = {str(uuid).lower() for uuid in properties.get("UUIDs", [])}
            if (selected_adapter(path) and properties.get("Paired")
                    and (A2DP_SOURCE_UUID in uuids or path == connected_path[0])):
                candidates.append((path != connected_path[0], path))
        return min(candidates)[1] if candidates else None

    def apply_phone_audio():
        wanted, path = phone_audio[0], audio_phone_path()
        if wanted is None or path is None:
            return False
        if wanted and any(t.startswith(path + "/") for t in transports):
            return False  # already playing here; another request only disturbs the links
        device = dbus.Interface(bus.get_object(BLUEZ, path), DEVICE_IFACE)
        action = "connect" if wanted else "disconnect"

        def failed(error):
            name = error.get_dbus_name() if hasattr(error, "get_dbus_name") else str(error)
            if not name.endswith(("AlreadyConnected", "NotConnected", "InProgress")):
                detail = error.get_dbus_message() if hasattr(error, "get_dbus_message") else ""
                log(f"phone audio {action}: {name}" + (f" ({detail})" if detail else ""))
            if wanted:
                retry_phone_audio()

        call = device.ConnectProfile if wanted else device.DisconnectProfile
        call(A2DP_SOURCE_UUID, reply_handler=lambda: log(f"phone audio {action}ed"),
             error_handler=failed)
        return False

    # The phone can refuse while its links are still settling (the iPhone
    # answers "Configuration not supported" or resets the first try), so keep
    # asking while it is connected here and the sound belongs on this PC.
    audio_retry = [None, 0]  # pending GLib source, attempts this connection

    def retry_phone_audio():
        if audio_retry[0] is not None or audio_retry[1] >= PHONE_AUDIO_TRIES:
            return
        audio_retry[1] += 1

        def again():
            audio_retry[0] = None
            path = audio_phone_path()
            if (phone_audio[0] and connected_path[0] and path
                    and not any(t.startswith(path + "/") for t in transports)):
                apply_phone_audio()
            return False

        audio_retry[0] = GLib.timeout_add_seconds(PHONE_AUDIO_RETRY_S, again)

    def reset_phone_audio_retry():
        if audio_retry[0] is not None:
            GLib.source_remove(audio_retry[0])
        audio_retry[0], audio_retry[1] = None, 0

    def set_phone_audio(wanted):
        changed = phone_audio[0] != wanted
        phone_audio[0] = wanted
        if changed:
            reset_phone_audio_retry()
            apply_phone_audio()
        return False

    def on_device_added(path, interfaces):
        if DEVICE_IFACE in interfaces:
            on_device_changed(DEVICE_IFACE, interfaces[DEVICE_IFACE], [], path=path)
        # The phone may start its audio by itself (it reconnects known speakers);
        # keep it on the phone unless Porthole chose this PC.
        if MEDIA_TRANSPORT_IFACE in interfaces and selected_adapter(path):
            transports.add(str(path))
            reset_phone_audio_retry()
            if phone_audio[0] is False:
                GLib.timeout_add(500, apply_phone_audio)

    def on_device_removed(path, interfaces):
        path = str(path)
        if MEDIA_TRANSPORT_IFACE in interfaces:
            transports.discard(path)
        if DEVICE_IFACE not in interfaces or not selected_adapter(path):
            return
        on_device_changed(DEVICE_IFACE, {"Connected": False}, [], path=path)
        devices.pop(path, None)
        hid_devices.discard(path)
        connection_observed.pop(path, None)
        if subscription_peer[0] == path:
            subscription_peer[0] = None
            for report in reports.values():
                report.subscribed_at = 0.0

    bus.add_signal_receiver(on_device_changed, dbus_interface=DBUS_PROP_IFACE,
                            signal_name="PropertiesChanged", path_keyword="path", bus_name=BLUEZ)
    bus.add_signal_receiver(on_device_added, dbus_interface=DBUS_OM_IFACE,
                            signal_name="InterfacesAdded", bus_name=BLUEZ)
    bus.add_signal_receiver(on_device_removed, dbus_interface=DBUS_OM_IFACE,
                            signal_name="InterfacesRemoved", bus_name=BLUEZ)
    app.on_device = on_gatt_device
    # Snapshot before service publication; newly arriving devices are observed
    # through InterfacesAdded. There is no blanket startup Disconnect timer.
    objects = dbus.Interface(bus.get_object(BLUEZ, "/"), DBUS_OM_IFACE).GetManagedObjects()
    for path, interfaces in objects.items():
        on_device_added(path, interfaces)
    register_app()

    frames, key_frames = queue.Queue(), queue.Queue()
    threading.Thread(target=stdin_reader, args=(frames, key_frames), daemon=True).start()
    key_sender = threading.Thread(target=sender, args=(key_frames, reports, loop, False),
                                  kwargs={"stopping": stopping}, daemon=True)
    key_sender.start()
    threading.Thread(target=sender, args=(frames, reports, loop),
                     kwargs={"wait_for": key_sender, "stopping": stopping,
                             "on_phone_audio": set_phone_audio}, daemon=True).start()
    for sig in (signal.SIGINT, signal.SIGTERM):
        GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, sig, loop.quit)

    emit("ready")
    try:
        loop.run()
    finally:
        stopping.set()
        release_reports(reports)
        if (connected_path[0] and connected_path[0] == subscription_peer[0]
                and bonded_and_trusted(connected_path[0])):
            try:
                device = dbus.Interface(bus.get_object(BLUEZ, connected_path[0]), DEVICE_IFACE)
                device.Disconnect(timeout=1.0)
            except dbus.exceptions.DBusException:
                pass
        for call in (lambda: adv_mgr.UnregisterAdvertisement(ADV_PATH),
                     lambda: gatt_mgr.UnregisterApplication(APP_PATH)):
            try:
                call()
            except dbus.exceptions.DBusException:
                pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
