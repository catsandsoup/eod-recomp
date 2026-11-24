"""Print cloud-init user data for the Garage Code service on an OCI Ubuntu VM."""

from pathlib import Path
from textwrap import indent


def main():
    relay = Path(__file__).with_name("garage_relay.py").read_text(encoding="utf-8")
    print("""#cloud-config
write_files:
  - path: /opt/eod/garage_relay.py
    owner: root:root
    permissions: '0644'
    content: |""")
    print(indent(relay.rstrip(), "      "))
    print("""  - path: /etc/systemd/system/eod-garage-relay.service
    owner: root:root
    permissions: '0644'
    content: |
      [Unit]
      Description=EOD Garage Code UDP relay
      After=network.target

      [Service]
      ExecStart=/usr/bin/python3 -B /opt/eod/garage_relay.py --port 27016
      DynamicUser=yes
      NoNewPrivileges=yes
      ProtectSystem=strict
      ProtectHome=yes
      PrivateTmp=yes
      Restart=always
      RestartSec=2s

      [Install]
      WantedBy=multi-user.target
bootcmd:
  - iptables -w -C INPUT -p udp --dport 27016 -j ACCEPT || iptables -w -I INPUT -p udp --dport 27016 -j ACCEPT
runcmd:
  - [systemctl, daemon-reload]
  - [systemctl, enable, --now, eod-garage-relay.service]""")


if __name__ == "__main__":
    main()
