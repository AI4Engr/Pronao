/* Sends WM_DELETE_WINDOW (what the window manager sends when the X is pressed) to every top-level window with exactly the given title.
 *   xclose "<title>"    prints how many windows were asked; exit 0 if at least one. Used by close_dialogs.sh; xdotool of older distributions has no windowquit. */
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <string.h>

static int titled(Display* d, Window w, const char* title, Atom utf8, Atom netName) {
    Atom type; int fmt; unsigned long n, after; unsigned char* data = NULL; int hit = 0;
    if (XGetWindowProperty(d, w, netName, 0, 1024, False, utf8, &type, &fmt, &n, &after, &data) == Success && data) { hit = strcmp((char*)data, title) == 0; XFree(data); if (hit) return 1; }
    char* name = NULL;
    if (XFetchName(d, w, &name) && name) { hit = strcmp(name, title) == 0; XFree(name); }
    return hit;
}

static int walk(Display* d, Window w, const char* title, Atom utf8, Atom netName, Atom protocols, Atom del, int depth) {
    int sent = 0;
    if (depth > 0 && titled(d, w, title, utf8, netName)) {
        Atom* list = NULL; int count = 0; int supports = 0;
        if (XGetWMProtocols(d, w, &list, &count)) { for (int i = 0; i < count; i++) if (list[i] == del) supports = 1; XFree(list); }
        if (supports) {
            XEvent e; memset(&e, 0, sizeof e);
            e.xclient.type = ClientMessage; e.xclient.window = w; e.xclient.message_type = protocols; e.xclient.format = 32;
            e.xclient.data.l[0] = del; e.xclient.data.l[1] = CurrentTime;
            XSendEvent(d, w, False, NoEventMask, &e);
            sent++;
        }
    }
    Window root, parent, *kids = NULL; unsigned int nkids = 0;
    if (XQueryTree(d, w, &root, &parent, &kids, &nkids)) { for (unsigned int i = 0; i < nkids; i++) sent += walk(d, kids[i], title, utf8, netName, protocols, del, depth + 1); if (kids) XFree(kids); }
    return sent;
}

int main(int argc, char** argv) {
    if (argc != 2) { fprintf(stderr, "usage: xclose <title>\n"); return 2; }
    Display* d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "no display\n"); return 2; }
    int sent = walk(d, DefaultRootWindow(d), argv[1], XInternAtom(d, "UTF8_STRING", False), XInternAtom(d, "_NET_WM_NAME", False), XInternAtom(d, "WM_PROTOCOLS", False),
                    XInternAtom(d, "WM_DELETE_WINDOW", False), 0);
    XFlush(d); XCloseDisplay(d);
    printf("%d\n", sent);
    return sent > 0 ? 0 : 1;
}
