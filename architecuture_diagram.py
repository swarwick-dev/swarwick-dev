import json
import html
import itertools
from copy import deepcopy
from pathlib import Path


RV_FILL = "#d5e8d4"
NATS_FILL = "#dae8fc"
SERVER_FILL = "#fff2cc"
APP_FILL = "#ffffff"
HEADER_FILL = "#e1d5e7"
LINE = "#666666"


class Drawio:
    def __init__(self, page_name):
        self.page_name = page_name
        self.ids = itertools.count(2)
        self.cells = [
            '<mxCell id="0"/>',
            '<mxCell id="1" parent="0"/>'
        ]

    def next_id(self):
        return str(next(self.ids))

    def add_rect(self, value, x, y, w, h, fill, stroke=LINE, rounded=True, font_size=12):
        cell_id = self.next_id()
        style = (
            f"rounded={1 if rounded else 0};whiteSpace=wrap;html=1;"
            f"fillColor={fill};strokeColor={stroke};fontSize={font_size};"
            "align=left;verticalAlign=top;spacing=8;"
        )
        self.cells.append(
            f'<mxCell id="{cell_id}" value="{html.escape(value, quote=False)}" '
            f'style="{style}" vertex="1" parent="1">'
            f'<mxGeometry x="{x}" y="{y}" width="{w}" height="{h}" as="geometry"/>'
            f'</mxCell>'
        )
        return cell_id

    def add_text(self, value, x, y, w, h, size=18, bold=False):
        cell_id = self.next_id()
        font_style = "1" if bold else "0"
        style = (
            "text;html=1;strokeColor=none;fillColor=none;"
            f"fontSize={size};fontStyle={font_style};"
            "align=left;verticalAlign=middle;"
        )
        self.cells.append(
            f'<mxCell id="{cell_id}" value="{html.escape(value, quote=False)}" '
            f'style="{style}" vertex="1" parent="1">'
            f'<mxGeometry x="{x}" y="{y}" width="{w}" height="{h}" as="geometry"/>'
            f'</mxCell>'
        )
        return cell_id

    def xml(self):
        cells = "\n".join(self.cells)
        return f"""<mxfile host="app.diagrams.net">
  <diagram name="{html.escape(self.page_name)}">
    <mxGraphModel dx="1600" dy="1000" grid="1" gridSize="10" guides="1" tooltips="1"
                  connect="1" arrows="1" fold="1" page="1" pageScale="1"
                  pageWidth="1900" pageHeight="1500" math="0" shadow="0">
      <root>
{cells}
      </root>
    </mxGraphModel>
  </diagram>
</mxfile>
"""


def transport_fill(value):
    text = value.upper()
    if text.startswith("NATS"):
        return NATS_FILL
    if text.startswith("RV"):
        return RV_FILL
    return "#f5f5f5"


def ensure_server(servers, server_name):
    if server_name not in servers:
        servers[server_name] = {
            "rvd_ports": [],
            "nats_ports": [],
            "applications": {}
        }

    servers[server_name].setdefault("rvd_ports", [])
    servers[server_name].setdefault("nats_ports", [])
    servers[server_name].setdefault("applications", {})


def apply_phase_changes(base_servers, phase):
    servers = deepcopy(base_servers)

    for server_name in servers:
        servers[server_name].setdefault("rvd_ports", [])
        servers[server_name].setdefault("nats_ports", [])
        servers[server_name].setdefault("applications", {})

    for server_name, server_data in phase.get("add_servers", {}).items():
        servers[server_name] = server_data
        ensure_server(servers, server_name)

    for server_name, server_changes in phase.get("server_overrides", {}).items():
        ensure_server(servers, server_name)

        for key, value in server_changes.items():
            servers[server_name][key] = value

    for server_name, apps in phase.get("add_applications", {}).items():
        ensure_server(servers, server_name)

        for app_name, app_data in apps.items():
            servers[server_name]["applications"][app_name] = app_data

    for path, new_value in phase.get("overrides", {}).items():
        try:
            server, app, role = path.split(".")
            servers[server]["applications"][app]["connections"][role] = new_value
        except KeyError as exc:
            raise KeyError(f"Invalid override path: {path}") from exc
        except ValueError as exc:
            raise ValueError(
                f"Invalid override path '{path}'. Expected Server.Application.Role"
            ) from exc

    return servers


def server_label(server_name, server):
    rvd_ports = server.get("rvd_ports", [])
    nats_ports = server.get("nats_ports", [])

    return (
        f"<b>{server_name}</b><br><br>"
        f"<b>RVD Process</b><br>"
        f"Ports: {', '.join(rvd_ports) if rvd_ports else '-'}<br><br>"
        f"<b>NATS Server</b><br>"
        f"Ports: {', '.join(nats_ports) if nats_ports else '-'}"
    )


def app_label(app_name, app_data):
    lines = [f"<b>{app_name}</b>"]

    status = app_data.get("status")
    if status:
        lines.append(f"Status: {status}")

    lines.append("<hr>")

    for role, value in app_data.get("connections", {}).items():
        lines.append(f"<b>{role}</b>: {value}")

    return "<br>".join(lines)


def render_current_architecture(data):
    d = Drawio("Current Architecture")

    d.add_text(
        "Current Architecture - Server / Application / Transport Inventory",
        40, 20, 1200, 40, 24, True
    )

    positions = {
        "Global_1": (40, 90),
        "UK_1": (40, 300),
        "NYK_1": (520, 300),
        "UK_2": (1000, 300),
        "UK_3": (1000, 650),
    }

    for server_name, server in data["servers"].items():
        x, y = positions.get(server_name, (40, 1000))
        apps = server.get("applications", {})

        server_h = 150 + max(1, len(apps)) * 145

        d.add_rect(
            server_label(server_name, server),
            x, y, 430, server_h, SERVER_FILL, font_size=14
        )

        app_y = y + 125

        for app_name, app_data in apps.items():
            d.add_rect(
                app_label(app_name, app_data),
                x + 20, app_y, 390, 120, APP_FILL
            )
            app_y += 140

    return d.xml()


def render_transport_phase(phase_key, phase, servers):
    d = Drawio(phase_key)

    d.add_text(phase["title"], 40, 20, 1200, 40, 24, True)
    d.add_text(phase.get("description", ""), 40, 60, 1300, 30, 14, False)

    d.add_rect("<b>Legend</b>", 1300, 40, 240, 130, "#ffffff")
    d.add_rect("TIBCO RV", 1320, 80, 190, 30, RV_FILL)
    d.add_rect("NATS", 1320, 120, 190, 30, NATS_FILL)

    positions = {
        "UK_1": (40, 130),
        "NYK_1": (520, 130),
        "UK_2": (1000, 230),
        "UK_3": (1000, 560),
        "Global_1": (40, 930),
    }

    for server_name, server in servers.items():
        x, y = positions.get(server_name, (40, 1100))
        apps = server.get("applications", {})

        server_h = 135 + max(1, len(apps)) * 155

        d.add_rect(
            server_label(server_name, server),
            x, y, 430, server_h, SERVER_FILL, font_size=14
        )

        app_y = y + 110

        for app_name, app_data in apps.items():
            status = app_data.get("status")
            title = f"<b>{app_name}</b>"
            if status:
                title += f"<br>Status: {status}"

            d.add_rect(
                title,
                x + 20,
                app_y,
                390,
                38,
                HEADER_FILL,
                font_size=12
            )

            row_y = app_y + 43

            for role, value in app_data.get("connections", {}).items():
                d.add_rect(
                    f"<b>{role}</b>: {value}",
                    x + 20,
                    row_y,
                    390,
                    28,
                    transport_fill(value),
                    font_size=11
                )
                row_y += 32

            app_y += 150

    return d.xml()


def main():
    input_path = Path("architecture_data.json")
    output_dir = Path("drawio_output")
    output_dir.mkdir(exist_ok=True)

    data = json.loads(input_path.read_text(encoding="utf-8"))

    current = render_current_architecture(data)
    (output_dir / "current_architecture.drawio").write_text(current, encoding="utf-8")

    for phase_key, phase in data["phases"].items():
        servers = apply_phase_changes(data["servers"], phase)
        xml = render_transport_phase(phase_key, phase, servers)
        (output_dir / f"{phase_key}.drawio").write_text(xml, encoding="utf-8")

    print("Generated Draw.io files in:", output_dir.resolve())


if __name__ == "__main__":
    main()