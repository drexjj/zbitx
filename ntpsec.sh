#!/bin/bash

# Ensure the script is run with sudo
if [ "$EUID" -ne 0 ]; then
  echo "Please run as root (use sudo)"
  exit
fi

echo "Updating package list..."
apt update -y

echo "Removing existing ntpsec installation..."
apt remove --purge -y ntpsec

echo "Cleaning up unnecessary packages..."
apt autoremove -y
apt autoclean -y

echo "Installing ntpsec..."
apt install -y ntpsec

echo "NTPsec has been reinstalled successfully."

# Enable and start the ntpsec service
echo "Enabling and starting NTPsec service..."
systemctl enable ntpd
systemctl start ntpd

echo "NTPsec service status:"
systemctl status ntpd
