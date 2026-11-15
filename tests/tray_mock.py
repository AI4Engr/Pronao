#!/usr/bin/env python3
"""A stand-in for a desktop's tray, for tests/run_tray_linux.sh: owns org.kde.StatusNotifierWatcher on the session bus and plays the part of the panel that shows the icon.
  argv[1]  log file: one line for each thing that happened ("registered NAME", "layout ...", "tooltip ...", "gone NAME")
  argv[2]  command file: the test writes one command per line, the mock runs it and removes the file
             layout            GetLayout of the dbusmenu: "layout label|enabled|checked ; ..." (separators as "-")
             activate          Activate(0, 0) on the item: a left click on the icon
             click N           Event(N, "clicked") on the dbusmenu: the user chose item number N
             tooltip           reads the item's ToolTip title and the size of its icon pixmap
             quit              ends
"""
import os, sys
import gi
gi.require_version('Gio', '2.0')
from gi.repository import Gio, GLib

XML = """<node><interface name='org.kde.StatusNotifierWatcher'>
<method name='RegisterStatusNotifierItem'><arg type='s' direction='in'/></method>
<method name='RegisterStatusNotifierHost'><arg type='s' direction='in'/></method>
<property name='RegisteredStatusNotifierItems' type='as' access='read'/><property name='IsStatusNotifierHostRegistered' type='b' access='read'/>
<property name='ProtocolVersion' type='i' access='read'/>
<signal name='StatusNotifierItemRegistered'><arg type='s'/></signal></interface></node>"""
log, cmdfile = sys.argv[1], sys.argv[2]
items = []
state = {'conn': None}

def say(line):
    with open(log, 'a') as f:
        f.write(line + '\n')

def on_call(conn, sender, path, iface, method, params, inv):
    if method == 'RegisterStatusNotifierItem':
        name = params.unpack()[0]
        items.append((name, sender))
        say('registered ' + name)
        conn.watch = conn.signal_subscribe('org.freedesktop.DBus', 'org.freedesktop.DBus', 'NameOwnerChanged', '/org/freedesktop/DBus', name, 0,
                                           lambda *a: (say('gone ' + name) if a[5][2] == '' else None))
    inv.return_value(None)

def on_prop(conn, sender, path, iface, name):
    if name == 'RegisteredStatusNotifierItems': return GLib.Variant('as', [i[0] for i in items])
    if name == 'IsStatusNotifierHostRegistered': return GLib.Variant('b', True)
    if name == 'ProtocolVersion': return GLib.Variant('i', 0)

def on_bus(conn, name):
    state['conn'] = conn
    node = Gio.DBusNodeInfo.new_for_xml(XML)
    conn.register_object('/StatusNotifierWatcher', node.interfaces[0], on_call, on_prop, None)

def call(path, iface, method, args, sig):
    name = items[-1][0]
    return state['conn'].call_sync(name, path, iface, method, args, GLib.VariantType(sig) if sig else None, Gio.DBusCallFlags.NONE, 3000, None)

def flatten(layout):
    nid, props, kids = layout
    out = []
    for k in kids:
        kid, kprops, _ = k
        if kprops.get('type') == 'separator': out.append('-')
        else: out.append('%s|%d|%d' % (kprops.get('label'), 1 if kprops.get('enabled', True) else 0, kprops.get('toggle-state', 0)))
    return ' ; '.join(out)

def tick():
    if not os.path.exists(cmdfile): return True
    cmd = open(cmdfile).read().strip()
    os.remove(cmdfile)
    try:
        if cmd == 'quit': loop.quit()
        elif cmd == 'layout':
            r = call('/MenuBar', 'com.canonical.dbusmenu', 'GetLayout', GLib.Variant('(iias)', (0, -1, [])), '(u(ia{sv}av))').unpack()
            say('layout ' + flatten(r[1]))
        elif cmd == 'activate':
            call('/StatusNotifierItem', 'org.kde.StatusNotifierItem', 'Activate', GLib.Variant('(ii)', (0, 0)), None); say('activated')
        elif cmd.startswith('click '):
            call('/MenuBar', 'com.canonical.dbusmenu', 'Event', GLib.Variant('(isvu)', (int(cmd.split()[1]), 'clicked', GLib.Variant('i', 0), 0)), None); say('clicked ' + cmd.split()[1])
        elif cmd == 'tooltip':
            tip = state['conn'].call_sync(items[-1][0], '/StatusNotifierItem', 'org.freedesktop.DBus.Properties', 'Get', GLib.Variant('(ss)', ('org.kde.StatusNotifierItem', 'ToolTip')), None, Gio.DBusCallFlags.NONE, 3000, None).unpack()[0]
            pix = state['conn'].call_sync(items[-1][0], '/StatusNotifierItem', 'org.freedesktop.DBus.Properties', 'Get', GLib.Variant('(ss)', ('org.kde.StatusNotifierItem', 'IconPixmap')), None, Gio.DBusCallFlags.NONE, 3000, None).unpack()[0]
            say('tooltip %s pixmaps=%s' % (tip[2], ','.join('%dx%d' % (p[0], p[1]) for p in pix)))
    except Exception as e:
        say('error %s: %s' % (cmd, e))
    return True

Gio.bus_own_name(Gio.BusType.SESSION, 'org.kde.StatusNotifierWatcher', Gio.BusNameOwnerFlags.NONE, on_bus, None, None)
loop = GLib.MainLoop()
GLib.timeout_add(100, tick)
open(log, 'a').close()
loop.run()
