#!/usr/bin/env python3
"""A stand-in for the desktop's notification service, for tests/run_notify_linux.sh. Owns org.freedesktop.Notifications on the session bus, writes one line per Notify call
to the file named by argv[1] ("notify <app> <urgency> <title> | <text> | actions=<n>"), and when argv[2] exists as a file emits ActionInvoked(<number>, "default") for the last
notification and deletes that file. Exits when argv[3] exists."""
import os, sys, time
import gi
gi.require_version('Gio', '2.0')
from gi.repository import Gio, GLib

XML = """<node><interface name='org.freedesktop.Notifications'>
<method name='Notify'><arg type='s' direction='in'/><arg type='u' direction='in'/><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='s' direction='in'/>
<arg type='as' direction='in'/><arg type='a{sv}' direction='in'/><arg type='i' direction='in'/><arg type='u' direction='out'/></method>
<method name='GetCapabilities'><arg type='as' direction='out'/></method>
<signal name='ActionInvoked'><arg type='u'/><arg type='s'/></signal></interface></node>"""
log, click, stop = sys.argv[1], sys.argv[2], sys.argv[3]
last = [0]
conn_ref = [None]

def handle(conn, sender, path, iface, method, params, inv):
    if method == 'Notify':
        app, rid, icon, title, text, actions, hints, timeout = params.unpack()
        last[0] += 1
        urgency = hints.get('urgency', -1)
        with open(log, 'a') as f:
            f.write('notify %s %s %s | %s | actions=%d\n' % (app, urgency, title, text, len(actions) // 2))
        inv.return_value(GLib.Variant('(u)', (last[0],)))
    elif method == 'GetCapabilities':
        inv.return_value(GLib.Variant('(as)', (['actions'],)))

def on_bus(conn, name):
    conn_ref[0] = conn
    node = Gio.DBusNodeInfo.new_for_xml(XML)
    conn.register_object('/org/freedesktop/Notifications', node.interfaces[0], handle, None, None)

def tick():
    if os.path.exists(stop):
        loop.quit(); return False
    if os.path.exists(click) and conn_ref[0] and last[0]:
        os.remove(click)
        conn_ref[0].emit_signal(None, '/org/freedesktop/Notifications', 'org.freedesktop.Notifications', 'ActionInvoked', GLib.Variant('(us)', (last[0], 'default')))
    return True

Gio.bus_own_name(Gio.BusType.SESSION, 'org.freedesktop.Notifications', Gio.BusNameOwnerFlags.NONE, on_bus, None, None)
loop = GLib.MainLoop()
GLib.timeout_add(100, tick)
open(log, 'a').close()
loop.run()
