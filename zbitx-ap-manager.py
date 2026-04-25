#!/usr/bin/env python3
import gi
gi.require_version("Gtk", "3.0")
from gi.repository import Gtk, GLib, Pango
import subprocess
import os

HOSTAPD_CONF = "/etc/hostapd/hostapd.conf"

class APManager(Gtk.Window):
    def __init__(self):
        super().__init__(title="zbitx AP Manager")
        self.set_default_size(400, 400)
        self.set_border_width(12)
        self.set_position(Gtk.WindowPosition.CENTER)

        vbox = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=10)
        self.add(vbox)

        # Title
        title = Gtk.Label()
        title.set_markup("<big><b>zbitx Access Point</b></big>")
        vbox.pack_start(title, False, False, 4)

        # Status section
        status_frame = Gtk.Frame(label="Status")
        status_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)
        status_box.set_border_width(8)
        status_frame.add(status_box)
        vbox.pack_start(status_frame, False, False, 0)

        self.status_label = Gtk.Label()
        self.status_label.set_xalign(0)
        status_box.pack_start(self.status_label, False, False, 0)

        self.info_label = Gtk.Label()
        self.info_label.set_xalign(0)
        status_box.pack_start(self.info_label, False, False, 0)

        # Password section
        pass_frame = Gtk.Frame(label="AP Password")
        pass_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
        pass_box.set_border_width(8)
        pass_frame.add(pass_box)
        vbox.pack_start(pass_frame, False, False, 0)

        self.pass_entry = Gtk.Entry()
        self.pass_entry.set_visibility(False)
        self.pass_entry.set_placeholder_text("Enter new password (min 8 chars)")
        pass_box.pack_start(self.pass_entry, True, True, 0)

        self.show_pass_btn = Gtk.ToggleButton(label="Show")
        self.show_pass_btn.connect("toggled", self.on_toggle_pass)
        pass_box.pack_start(self.show_pass_btn, False, False, 0)

        self.change_pass_btn = Gtk.Button(label="Change")
        self.change_pass_btn.connect("clicked", self.on_change_password)
        pass_box.pack_start(self.change_pass_btn, False, False, 0)

        # Load current password
        self.load_current_password()

        # Clients section
        clients_frame = Gtk.Frame(label="Connected Clients")
        clients_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=4)
        clients_box.set_border_width(8)
        clients_frame.add(clients_box)
        vbox.pack_start(clients_frame, True, True, 0)

        scroll = Gtk.ScrolledWindow()
        scroll.set_min_content_height(100)
        self.clients_label = Gtk.Label()
        self.clients_label.set_xalign(0)
        self.clients_label.set_yalign(0)
        self.clients_label.set_line_wrap(True)
        scroll.add(self.clients_label)
        clients_box.pack_start(scroll, True, True, 0)

        # Buttons
        btn_box = Gtk.Box(spacing=8)
        vbox.pack_start(btn_box, False, False, 0)

        self.start_btn = Gtk.Button(label="Start AP")
        self.start_btn.connect("clicked", self.on_start)
        btn_box.pack_start(self.start_btn, True, True, 0)

        self.stop_btn = Gtk.Button(label="Stop AP")
        self.stop_btn.connect("clicked", self.on_stop)
        btn_box.pack_start(self.stop_btn, True, True, 0)

        refresh_btn = Gtk.Button(label="Refresh")
        refresh_btn.connect("clicked", lambda w: self.update_status())
        btn_box.pack_start(refresh_btn, True, True, 0)

        close_btn = Gtk.Button(label="Close")
        close_btn.connect("clicked", lambda w: self.destroy())
        btn_box.pack_start(close_btn, True, True, 0)

        self.update_status()
        GLib.timeout_add_seconds(5, self.auto_refresh)

    def run_cmd(self, cmd):
        try:
            r = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=10)
            return r.stdout.strip(), r.returncode
        except:
            return "", 1

    def is_ap_running(self):
        _, rc = self.run_cmd("systemctl is-active hostapd")
        return rc == 0

    def load_current_password(self):
        out, rc = self.run_cmd(f"grep ^wpa_passphrase= {HOSTAPD_CONF}")
        if rc == 0 and "=" in out:
            self.pass_entry.set_text(out.split("=", 1)[1])

    def on_toggle_pass(self, widget):
        self.pass_entry.set_visibility(widget.get_active())
        widget.set_label("Hide" if widget.get_active() else "Show")

    def on_change_password(self, widget):
        new_pass = self.pass_entry.get_text().strip()
        if len(new_pass) < 8:
            dialog = Gtk.MessageDialog(
                parent=self, flags=0, message_type=Gtk.MessageType.ERROR,
                buttons=Gtk.ButtonsType.OK,
                text="Password must be at least 8 characters.")
            dialog.run()
            dialog.destroy()
            return

        # Confirm
        dialog = Gtk.MessageDialog(
            parent=self, flags=0, message_type=Gtk.MessageType.QUESTION,
            buttons=Gtk.ButtonsType.YES_NO,
            text=f"Change AP password to \"{new_pass}\"?\n\nThis will restart the access point.")
        response = dialog.run()
        dialog.destroy()
        if response != Gtk.ResponseType.YES:
            return

        # Update hostapd.conf
        _, rc = self.run_cmd(
            f"sudo sed -i s/^wpa_passphrase=.*/wpa_passphrase={new_pass}/ {HOSTAPD_CONF}")
        if rc != 0:
            self.show_msg("Error", "Failed to update password.", Gtk.MessageType.ERROR)
            return

        # Restart hostapd
        self.run_cmd("sudo systemctl restart hostapd")
        GLib.timeout_add(2000, self._after_pass_change)

    def _after_pass_change(self):
        self.update_status()
        self.show_msg("Success", "AP password changed and hostapd restarted.", Gtk.MessageType.INFO)
        return False

    def show_msg(self, title, text, msg_type):
        dialog = Gtk.MessageDialog(
            parent=self, flags=0, message_type=msg_type,
            buttons=Gtk.ButtonsType.OK, text=text)
        dialog.set_title(title)
        dialog.run()
        dialog.destroy()

    def get_clients(self):
        out, _ = self.run_cmd("iw dev uap0 station dump")
        clients = []
        mac = None
        for line in out.splitlines():
            if line.startswith("Station"):
                mac = line.split()[1]
            if mac and "signal:" in line:
                signal = line.strip()
                clients.append(f"{mac}  ({signal})")
                mac = None
            elif mac and line.strip() == "":
                clients.append(mac)
                mac = None
        if mac:
            clients.append(mac)
        return clients

    def update_status(self):
        running = self.is_ap_running()
        if running:
            self.status_label.set_markup("AP Status: <b><span foreground=green>Running</span></b>")
            ssid_out, _ = self.run_cmd(f"grep ^ssid= {HOSTAPD_CONF}")
            ip_out, _ = self.run_cmd("ip -4 addr show uap0 | grep inet")
            ssid = ssid_out.replace("ssid=", "") if ssid_out else "zbitx"
            ip_addr = ip_out.strip().split()[1] if ip_out.strip() else "N/A"
            self.info_label.set_markup(f"SSID: <b>{ssid}</b>    IP: <b>{ip_addr}</b>")
            clients = self.get_clients()
            if clients:
                self.clients_label.set_text("\n".join(clients))
            else:
                self.clients_label.set_text("No clients connected")
            self.start_btn.set_sensitive(False)
            self.stop_btn.set_sensitive(True)
        else:
            self.status_label.set_markup("AP Status: <b><span foreground=red>Stopped</span></b>")
            self.info_label.set_text("")
            self.clients_label.set_text("")
            self.start_btn.set_sensitive(True)
            self.stop_btn.set_sensitive(False)

    def auto_refresh(self):
        self.update_status()
        return True

    def on_start(self, widget):
        self.run_cmd("sudo systemctl start hostapd")
        GLib.timeout_add(2000, self.update_status)

    def on_stop(self, widget):
        self.run_cmd("sudo systemctl stop hostapd")
        GLib.timeout_add(2000, self.update_status)

win = APManager()
win.connect("destroy", Gtk.main_quit)
win.show_all()
Gtk.main()
